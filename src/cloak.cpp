#include "cloak.h"

#include <Arduino.h>
#include <string.h>
#include <time.h>
#include <esp_random.h>
#include <mbedtls/md.h>

namespace cloak {

namespace {

/**
 * НАСТЕННОЕ время в миллисекундах — не время от запуска.
 *
 * Номер эпохи обе стороны считают независимо, и считают его именно от календарного
 * времени. Возьми я время от включения платы — эпохи не совпали бы никогда, и ни один
 * пакет не был бы понят. Время приходит по сети при подключении к Wi-Fi.
 *
 * Пока время не получено, возвращаем ноль: в этом состоянии обфусцированный обмен
 * невозможен, и лучше честно молчать, чем слать пакеты, которые никто не разберёт.
 */
uint64_t wallMs() {
    time_t t = time(nullptr);
    // Меньше этой отметки — часы ещё не выставлены: система начинает счёт с 1970 года.
    if (t < 1700000000L) return 0;
    return uint64_t(t) * 1000ULL;
}

constexpr size_t kSeedLen = 8;
constexpr size_t kTagLen  = 4;    // короткая метка: отсеять чужое, не раздувая пакет
constexpr size_t kMaxPad  = 32;

/** HMAC на SHA-256 — им считаются и ключи, и гамма, и метка. */
void hmac(const uint8_t* key, size_t keyLen,
          const uint8_t* const* parts, const size_t* lens, size_t n,
          uint8_t out[32]) {
    const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    static const uint8_t kZero[32] = {};
    if (mbedtls_md_setup(&ctx, md, 1) == 0) {
        mbedtls_md_hmac_starts(&ctx, keyLen ? key : kZero, keyLen ? keyLen : 32);
        for (size_t i = 0; i < n; ++i) mbedtls_md_hmac_update(&ctx, parts[i], lens[i]);
        mbedtls_md_hmac_finish(&ctx, out);
    }
    mbedtls_md_free(&ctx);
}

/** Ключ из частей. Метка «voile-cloak-v1» обязана совпадать с телефонной. */
void keyFrom(const uint8_t* const* parts, const size_t* lens, size_t n, uint8_t out[32]) {
    static const char kLabel[] = "voile-cloak-v1";
    hmac(reinterpret_cast<const uint8_t*>(kLabel), sizeof(kLabel) - 1, parts, lens, n, out);
}

void epochKey(const uint8_t baseKey[32], uint64_t ep, uint8_t out[32]) {
    static const char kEpoch[] = "epoch";
    const uint8_t e[4] = {
        uint8_t(ep >> 24), uint8_t(ep >> 16), uint8_t(ep >> 8), uint8_t(ep)
    };
    const uint8_t* parts[3] = {
        baseKey, reinterpret_cast<const uint8_t*>(kEpoch), e
    };
    const size_t lens[3] = {32, sizeof(kEpoch) - 1, 4};
    keyFrom(parts, lens, 3, out);
}

/** Гамма: HMAC(ключ, семя ‖ счётчик) блоками по 32 байта. */
void stream(const uint8_t key[32], const uint8_t seed[kSeedLen], uint8_t* out, size_t len) {
    size_t pos = 0;
    uint8_t ctr = 0;
    while (pos < len) {
        uint8_t block[32];
        const uint8_t* parts[2] = {seed, &ctr};
        const size_t lens[2] = {kSeedLen, 1};
        hmac(key, 32, parts, lens, 2, block);
        const size_t take = (len - pos) < 32 ? (len - pos) : 32;
        memcpy(out + pos, block, take);
        pos += take; ++ctr;
    }
}

void tagOf(const uint8_t key[32], const uint8_t seed[kSeedLen],
           const uint8_t* payload, size_t len, uint8_t out[kTagLen]) {
    uint8_t full[32];
    const uint8_t* parts[2] = {seed, payload};
    const size_t lens[2] = {kSeedLen, len};
    hmac(key, 32, parts, lens, 2, full);
    memcpy(out, full, kTagLen);
}

/**
 * Внешний вид пакета. Профиль выводится из ключа эпохи, поэтому посторонний его не
 * предскажет, а обе стороны считают одинаково.
 *
 * QUIC берётся чаще: такой заголовок лучше всего сливается с обычным трафиком.
 */
bool quicProfile(const uint8_t baseKey[32], uint64_t ep) {
    uint8_t k[32];
    epochKey(baseKey, ep, k);
    return (k[0] % 4) != 0;
}

size_t unwrap(const uint8_t key[32], const uint8_t* buf, size_t len,
              uint8_t* out, size_t outCap) {
    if (len < kSeedLen + kTagLen + 2) return 0;
    const uint8_t* seed = buf;
    const size_t innerLen = len - kSeedLen;
    if (innerLen > 1600) return 0;

    uint8_t inner[1600];
    memcpy(inner, buf + kSeedLen, innerLen);

    uint8_t ks[1600];
    stream(key, seed, ks, innerLen);
    for (size_t i = 0; i < innerLen; ++i) inner[i] ^= ks[i];

    const size_t plen = (size_t(inner[kTagLen]) << 8) | inner[kTagLen + 1];
    if (plen == 0 || kTagLen + 2 + plen > innerLen || plen > outCap) return 0;

    uint8_t want[kTagLen];
    tagOf(key, seed, inner + kTagLen + 2, plen, want);
    if (memcmp(want, inner, kTagLen) != 0) return 0;   // не наш ключ или мусор

    memcpy(out, inner + kTagLen + 2, plen);
    return plen;
}

}  // namespace

uint64_t epoch(uint64_t nowMs) { return nowMs / kEpochMs; }

const uint8_t* discoveryKey() {
    static uint8_t key[32];
    static bool ready = false;
    if (!ready) {
        static const char kDisc[] = "discovery";
        const uint8_t* parts[1] = {reinterpret_cast<const uint8_t*>(kDisc)};
        const size_t lens[1] = {sizeof(kDisc) - 1};
        keyFrom(parts, lens, 1, key);
        ready = true;
    }
    return key;
}

void keyForNode(const uint8_t nodeId[20], uint8_t out[32]) {
    static const char kNode[] = "node";
    const uint8_t* parts[2] = {reinterpret_cast<const uint8_t*>(kNode), nodeId};
    const size_t lens[2] = {sizeof(kNode) - 1, 20};
    keyFrom(parts, lens, 2, out);
}

size_t seal(const uint8_t baseKey[32], const uint8_t* payload, size_t len,
            uint8_t* out, size_t outCap) {
    const uint64_t now = wallMs();
    if (now == 0) return 0;              // часы не выставлены — обмен невозможен
    const uint64_t ep = epoch(now);
    uint8_t key[32];
    epochKey(baseKey, ep, key);

    uint8_t seed[kSeedLen];
    esp_fill_random(seed, sizeof(seed));
    const size_t pad = esp_random() % kMaxPad;

    const size_t innerLen = kTagLen + 2 + len + pad;
    const size_t total = (quicProfile(baseKey, ep) ? 1 : 0) + kSeedLen + innerLen;
    if (total > outCap || innerLen > 1600) return 0;

    uint8_t inner[1600];
    tagOf(key, seed, payload, len, inner);
    inner[kTagLen] = uint8_t(len >> 8);
    inner[kTagLen + 1] = uint8_t(len & 0xFF);
    memcpy(inner + kTagLen + 2, payload, len);
    if (pad) esp_fill_random(inner + kTagLen + 2 + len, pad);

    uint8_t ks[1600];
    stream(key, seed, ks, innerLen);
    for (size_t i = 0; i < innerLen; ++i) inner[i] ^= ks[i];

    size_t o = 0;
    if (quicProfile(baseKey, ep)) {
        // Короткий заголовок QUIC: старший бит 0, следующий 1, остальное шумит.
        out[o++] = uint8_t(0x40 | (esp_random() & 0x3F));
    }
    memcpy(out + o, seed, kSeedLen); o += kSeedLen;
    memcpy(out + o, inner, innerLen); o += innerLen;
    return o;
}

size_t open(const uint8_t baseKey[32], const uint8_t* buf, size_t len,
            uint8_t* out, size_t outCap) {
    const uint64_t w = wallMs();
    if (w == 0) return 0;
    const uint64_t now = epoch(w);
    // Текущая эпоха и соседние: часы у сторон расходятся, и пакет мог уйти в одной
    // эпохе, а прийти в другой. Без этого связь рвалась бы каждые двадцать минут ровно
    // на границе — и причину искали бы долго.
    for (int d = 0; d <= 1; ++d) {
        for (int sign = 1; sign >= -1; sign -= 2) {
            const uint64_t ep = now + uint64_t(int64_t(d * sign));
            uint8_t key[32];
            epochKey(baseKey, ep, key);
            // Пробуем оба вида: с заголовком-обманкой и без него.
            size_t n = unwrap(key, buf, len, out, outCap);
            if (n) return n;
            if (len > 1) {
                n = unwrap(key, buf + 1, len - 1, out, outCap);
                if (n) return n;
            }
            if (d == 0) break;
        }
    }
    return 0;
}

}  // namespace cloak
