// Криптография на mbedTLS — реализация для платы.
//
// Та же кривая и тот же вывод ключей, что в телефонной версии: P-256, HKDF на SHA-256,
// AES-256-GCM. У ESP32-S3 AES и SHA ускорены аппаратно, и mbedTLS этим пользуется сам.
//
// На компьютере вместо этого файла подключается test/crypto_test_backend.cpp — сам
// храповик при этом один и тот же, поэтому его логика проверяется тестами до заливки.
#include "voile_crypto.h"

#include <string.h>

#include <mbedtls/ecdh.h>
#include <mbedtls/ecp.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/md.h>
#include <mbedtls/gcm.h>

namespace voile {

namespace {

// Источник случайности заводим один раз: инициализация энтропии дорогая, а на каждое
// сообщение генерируется эфемерная пара.
mbedtls_entropy_context  g_entropy;
mbedtls_ctr_drbg_context g_drbg;
bool g_rngReady = false;

bool rngInit() {
    if (g_rngReady) return true;
    mbedtls_entropy_init(&g_entropy);
    mbedtls_ctr_drbg_init(&g_drbg);
    static const char kSeed[] = "voile-tdeck";
    const int rc = mbedtls_ctr_drbg_seed(&g_drbg, mbedtls_entropy_func, &g_entropy,
                                         reinterpret_cast<const unsigned char*>(kSeed),
                                         sizeof(kSeed) - 1);
    g_rngReady = (rc == 0);
    return g_rngReady;
}

}  // namespace

bool genKeyPair(KeyPair& out) {
    if (!rngInit()) return false;

    mbedtls_ecp_group grp;
    mbedtls_mpi d;
    mbedtls_ecp_point Q;
    mbedtls_ecp_group_init(&grp);
    mbedtls_mpi_init(&d);
    mbedtls_ecp_point_init(&Q);

    bool ok = false;
    do {
        if (mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1) != 0) break;
        if (mbedtls_ecp_gen_keypair(&grp, &d, &Q, mbedtls_ctr_drbg_random, &g_drbg) != 0) break;
        if (mbedtls_mpi_write_binary(&d, out.priv, sizeof(out.priv)) != 0) break;

        // Точка в СЖАТОМ виде: 33 байта вместо 65. При пределе пакета LoRa в 255 байт
        // эта разница определяет, влезет ли вообще что-то полезное.
        size_t olen = 0;
        if (mbedtls_ecp_point_write_binary(&grp, &Q, MBEDTLS_ECP_PF_COMPRESSED,
                                           &olen, out.pubComp, sizeof(out.pubComp)) != 0) break;
        ok = (olen == kPubComp);
    } while (false);

    mbedtls_ecp_point_free(&Q);
    mbedtls_mpi_free(&d);
    mbedtls_ecp_group_free(&grp);
    return ok;
}

bool ecdh(const uint8_t priv[32], const uint8_t peerPubComp[kPubComp],
          uint8_t out[kSharedLen]) {
    if (!rngInit()) return false;

    mbedtls_ecp_group grp;
    mbedtls_mpi d, z;
    mbedtls_ecp_point Q;
    mbedtls_ecp_group_init(&grp);
    mbedtls_mpi_init(&d);
    mbedtls_mpi_init(&z);
    mbedtls_ecp_point_init(&Q);

    bool ok = false;
    do {
        if (mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1) != 0) break;
        if (mbedtls_mpi_read_binary(&d, priv, 32) != 0) break;
        // Разбор сжатой точки: восстанавливает координату Y из X и признака чётности.
        if (mbedtls_ecp_point_read_binary(&grp, &Q, peerPubComp, kPubComp) != 0) break;
        // Проверка, что точка лежит на кривой, обязательна: без неё подсунутая точка
        // малого порядка раскрыла бы наш секретный ключ.
        if (mbedtls_ecp_check_pubkey(&grp, &Q) != 0) break;
        if (mbedtls_ecdh_compute_shared(&grp, &z, &Q, &d,
                                        mbedtls_ctr_drbg_random, &g_drbg) != 0) break;
        ok = (mbedtls_mpi_write_binary(&z, out, kSharedLen) == 0);
    } while (false);

    mbedtls_ecp_point_free(&Q);
    mbedtls_mpi_free(&z);
    mbedtls_mpi_free(&d);
    mbedtls_ecp_group_free(&grp);
    return ok;
}

/**
 * HKDF на SHA-256 — собственная реализация по RFC 5869.
 *
 * Готовый mbedtls_hkdf использовать НЕЛЬЗЯ: в сборке mbedTLS, которая идёт с фреймворком
 * ESP32, эта функция отключена. Заголовок при этом на месте, поэтому код компилируется и
 * падает только на сборке, с невнятным «undefined reference».
 *
 * Собрать её из HMAC — четыре десятка строк, а HMAC доступен и ускорен аппаратно.
 * Формула обязана совпадать с телефонной версией побайтово, иначе стороны выведут разные
 * ключи и не расшифруют друг друга.
 */
void hkdf(const uint8_t* secret, size_t secretLen,
          const uint8_t* salt, size_t saltLen,
          const uint8_t* info, size_t infoLen,
          uint8_t* out, size_t outLen) {
    const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!md) return;
    const size_t hlen = 32;

    // Шаг 1 — извлечение: PRK = HMAC(соль, секрет). Пустая соль по стандарту заменяется
    // нулями длиной в размер хеша.
    static const uint8_t kZeroSalt[32] = {};
    uint8_t prk[32];
    mbedtls_md_hmac(md,
                    salt ? salt : kZeroSalt, salt ? saltLen : sizeof(kZeroSalt),
                    secret, secretLen, prk);

    // Шаг 2 — расширение: T(i) = HMAC(PRK, T(i-1) | info | i), склеиваем до нужной длины.
    uint8_t t[32];
    size_t tlen = 0, done = 0;
    uint8_t counter = 1;

    while (done < outLen) {
        mbedtls_md_context_t ctx;
        mbedtls_md_init(&ctx);
        if (mbedtls_md_setup(&ctx, md, 1) != 0) { mbedtls_md_free(&ctx); return; }
        mbedtls_md_hmac_starts(&ctx, prk, hlen);
        if (tlen) mbedtls_md_hmac_update(&ctx, t, tlen);
        if (info && infoLen) mbedtls_md_hmac_update(&ctx, info, infoLen);
        mbedtls_md_hmac_update(&ctx, &counter, 1);
        mbedtls_md_hmac_finish(&ctx, t);
        mbedtls_md_free(&ctx);

        tlen = hlen;
        ++counter;
        const size_t take = (outLen - done) < hlen ? (outLen - done) : hlen;
        memcpy(out + done, t, take);
        done += take;
    }
}

namespace {

/**
 * Одноразовый вектор из счётчика.
 *
 * В кадре его нет намеренно: 12 байт на каждом пакете при пределе 255 — непозволительно.
 * Получатель собирает тот же вектор из номера сообщения, который и так есть в заголовке.
 * Повтор счётчика на одном ключе недопустим, но здесь это исключено: ключ у каждого
 * сообщения свой — храповик меняет его на каждом шаге.
 */
void nonceFrom(uint64_t counter, uint8_t out[12]) {
    memset(out, 0, 12);
    for (int i = 0; i < 8; ++i) out[4 + i] = uint8_t(counter >> (56 - i * 8));
}

}  // namespace

bool seal(const uint8_t key[kKeyLen], uint64_t nonceCounter,
          const uint8_t* aad, size_t aadLen,
          const uint8_t* plain, size_t plainLen,
          uint8_t* out, uint8_t tag[8]) {
    uint8_t nonce[12];
    nonceFrom(nonceCounter, nonce);

    mbedtls_gcm_context g;
    mbedtls_gcm_init(&g);
    bool ok = false;
    if (mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, key, kKeyLen * 8) == 0) {
        // Тег укорочен до 8 байт — см. voile_frame.h: при таких объёмах и скорости
        // перебора по радио 64 бит достаточно, а экономия ощутима на каждом пакете.
        ok = mbedtls_gcm_crypt_and_tag(&g, MBEDTLS_GCM_ENCRYPT, plainLen,
                                       nonce, sizeof(nonce), aad, aadLen,
                                       plain, out, 8, tag) == 0;
    }
    mbedtls_gcm_free(&g);
    return ok;
}

bool open(const uint8_t key[kKeyLen], uint64_t nonceCounter,
          const uint8_t* aad, size_t aadLen,
          const uint8_t* cipher, size_t cipherLen,
          const uint8_t tag[8], uint8_t* out) {
    uint8_t nonce[12];
    nonceFrom(nonceCounter, nonce);

    mbedtls_gcm_context g;
    mbedtls_gcm_init(&g);
    bool ok = false;
    if (mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, key, kKeyLen * 8) == 0) {
        ok = mbedtls_gcm_auth_decrypt(&g, cipherLen, nonce, sizeof(nonce),
                                      aad, aadLen, tag, 8, cipher, out) == 0;
    }
    mbedtls_gcm_free(&g);
    return ok;
}

}  // namespace voile
