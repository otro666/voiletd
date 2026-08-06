#include "schnorr.h"

#include <string.h>
#include <esp_random.h>
#include <mbedtls/ecp.h>
#include <mbedtls/bignum.h>
#include <mbedtls/sha256.h>

// В версии библиотеки, что идёт с этим фреймворком, поля точки называются просто X, Y, Z.
// В более новых их спрятали и обращаются через макрос. Пишем через макрос, а если его
// нет — подставляем имя как есть: тогда код собирается с обеими версиями, и переход на
// новую не потребует правок.
#if !defined(MBEDTLS_PRIVATE)
#define MBEDTLS_PRIVATE(field) field
#endif

namespace schnorr {

namespace {

/**
 * Хеш с меткой назначения.
 *
 * Считается как SHA256(SHA256(метка) ‖ SHA256(метка) ‖ данные). Двойное повторение хеша
 * метки — не описка, а часть стандарта: так значение метки заполняет первый блок хеша
 * целиком, и подобрать столкновение между разными назначениями нельзя.
 */
void taggedHash(const char* tag, const uint8_t* const* parts, const size_t* lens,
                size_t n, uint8_t out[32]) {
    uint8_t th[32];
    mbedtls_sha256(reinterpret_cast<const uint8_t*>(tag), strlen(tag), th, 0);

    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, th, 32);
    mbedtls_sha256_update(&ctx, th, 32);
    for (size_t i = 0; i < n; ++i) mbedtls_sha256_update(&ctx, parts[i], lens[i]);
    mbedtls_sha256_finish(&ctx, out);
    mbedtls_sha256_free(&ctx);
}

/** Заглушка генератора: точка умножения требует источник случайности, но здесь все
 *  значения уже определены, и случайность не нужна. */
int noRng(void*, unsigned char* out, size_t len) {
    esp_fill_random(out, len);
    return 0;
}

struct Curve {
    mbedtls_ecp_group grp;
    Curve() {
        mbedtls_ecp_group_init(&grp);
        // Та же кривая, что у Nostr и биткойна. У неё нет отдельного значения знака,
        // поэтому дальше всюду приходится приводить точки к чётному варианту.
        mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256K1);
    }
    ~Curve() { mbedtls_ecp_group_free(&grp); }
};

/** Точка чётная? По стандарту работаем только с такими. */
bool isEven(const mbedtls_ecp_point& p) {
    return mbedtls_mpi_get_bit(&p.MBEDTLS_PRIVATE(Y), 0) == 0;
}

}  // namespace

bool publicKey(const uint8_t sk[32], uint8_t xOnly[32]) {
    Curve c;
    mbedtls_mpi d;  mbedtls_mpi_init(&d);
    mbedtls_ecp_point P; mbedtls_ecp_point_init(&P);
    bool ok = false;

    if (mbedtls_mpi_read_binary(&d, sk, 32) == 0 &&
        mbedtls_ecp_mul(&c.grp, &P, &d, &c.grp.G, noRng, nullptr) == 0 &&
        mbedtls_mpi_write_binary(&P.MBEDTLS_PRIVATE(X), xOnly, 32) == 0) {
        ok = true;
    }

    mbedtls_mpi_free(&d);
    mbedtls_ecp_point_free(&P);
    return ok;
}

bool sign(const uint8_t msg32[32], const uint8_t sk[32], uint8_t sig[64]) {
    Curve c;
    bool ok = false;

    mbedtls_mpi d, k, e, s, n;
    mbedtls_ecp_point P, R;
    mbedtls_mpi_init(&d); mbedtls_mpi_init(&k); mbedtls_mpi_init(&e);
    mbedtls_mpi_init(&s); mbedtls_mpi_init(&n);
    mbedtls_ecp_point_init(&P); mbedtls_ecp_point_init(&R);

    do {
        if (mbedtls_mpi_read_binary(&d, sk, 32) != 0) break;
        mbedtls_mpi_copy(&n, &c.grp.N);

        // Открытый ключ и приведение закрытого к чётному варианту. Если точка нечётная,
        // берём вместо ключа его дополнение — иначе проверяющий получит другую точку.
        if (mbedtls_ecp_mul(&c.grp, &P, &d, &c.grp.G, noRng, nullptr) != 0) break;
        if (!isEven(P)) mbedtls_mpi_sub_mpi(&d, &n, &d);

        uint8_t px[32];
        if (mbedtls_mpi_write_binary(&P.MBEDTLS_PRIVATE(X), px, 32) != 0) break;

        // Одноразовое число.
        //
        // Случайность НЕ подаётся отдельным куском, а складывается с ключом исключающим
        // ИЛИ — так велит стандарт. Я сперва сделал иначе, и подпись не сошлась с
        // эталонными векторами: проверяющий получал другое значение и отвергал её молча.
        // Смысл именно такого порядка в том, что при отказе источника случайности подпись
        // остаётся верной, просто перестаёт быть непредсказуемой.
        uint8_t aux[32];
        esp_fill_random(aux, sizeof(aux));

        uint8_t auxHash[32];
        {
            const uint8_t* parts[1] = {aux};
            const size_t lens[1] = {32};
            taggedHash("BIP0340/aux", parts, lens, 1, auxHash);
        }

        uint8_t dbytes[32];
        mbedtls_mpi_write_binary(&d, dbytes, 32);
        uint8_t t[32];
        for (int i = 0; i < 32; ++i) t[i] = uint8_t(dbytes[i] ^ auxHash[i]);

        uint8_t kh[32];
        {
            const uint8_t* parts[3] = {t, px, msg32};
            const size_t lens[3] = {32, 32, 32};
            taggedHash("BIP0340/nonce", parts, lens, 3, kh);
        }
        if (mbedtls_mpi_read_binary(&k, kh, 32) != 0) break;
        mbedtls_mpi_mod_mpi(&k, &k, &n);
        if (mbedtls_mpi_cmp_int(&k, 0) == 0) break;

        // Точка R и приведение её к чётной — по той же причине, что и ключа.
        if (mbedtls_ecp_mul(&c.grp, &R, &k, &c.grp.G, noRng, nullptr) != 0) break;
        if (!isEven(R)) mbedtls_mpi_sub_mpi(&k, &n, &k);

        uint8_t rx[32];
        if (mbedtls_mpi_write_binary(&R.MBEDTLS_PRIVATE(X), rx, 32) != 0) break;

        // Вызов: e = хеш(R ‖ ключ ‖ сообщение)
        uint8_t eh[32];
        {
            const uint8_t* parts[3] = {rx, px, msg32};
            const size_t lens[3] = {32, 32, 32};
            taggedHash("BIP0340/challenge", parts, lens, 3, eh);
        }
        if (mbedtls_mpi_read_binary(&e, eh, 32) != 0) break;
        mbedtls_mpi_mod_mpi(&e, &e, &n);

        // s = k + e·d  (по модулю порядка)
        if (mbedtls_mpi_mul_mpi(&s, &e, &d) != 0) break;
        mbedtls_mpi_mod_mpi(&s, &s, &n);
        mbedtls_mpi_add_mpi(&s, &s, &k);
        mbedtls_mpi_mod_mpi(&s, &s, &n);

        memcpy(sig, rx, 32);
        if (mbedtls_mpi_write_binary(&s, sig + 32, 32) != 0) break;
        ok = true;
    } while (false);

    mbedtls_mpi_free(&d); mbedtls_mpi_free(&k); mbedtls_mpi_free(&e);
    mbedtls_mpi_free(&s); mbedtls_mpi_free(&n);
    mbedtls_ecp_point_free(&P); mbedtls_ecp_point_free(&R);
    return ok;
}

}  // namespace schnorr
