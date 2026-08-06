#include "contacts.h"

#include <string.h>
#include <stdio.h>

namespace contacts {

namespace {
Contact g_list[kMaxContacts];
size_t  g_count = 0;
}

/**
 * SHA-1 — только ради совместимости с телефонной версией.
 *
 * Для новой криптографии он давно непригоден, и в самом протоколе не используется нигде.
 * Но комната присутствия в телефоне выводится именно через него, и формула зафиксирована
 * во всех установленных версиях: сменить её значило бы разом порвать связь со всеми.
 * Здесь SHA-1 не защищает ничего — он лишь даёт обеим сторонам одно и то же число.
 */
static void sha1(const uint8_t* data, size_t len, uint8_t out[20]) {
    uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    const uint64_t bits = uint64_t(len) * 8;
    const size_t total = ((len + 8) / 64 + 1) * 64;

    auto rol = [](uint32_t v, int c) { return (v << c) | (v >> (32 - c)); };
    uint8_t blk[64];

    for (size_t off = 0; off < total; off += 64) {
        for (size_t i = 0; i < 64; ++i) {
            const size_t idx = off + i;
            if (idx < len)            blk[i] = data[idx];
            else if (idx == len)      blk[i] = 0x80;
            else if (idx < total - 8) blk[i] = 0;
            else                      blk[i] = uint8_t(bits >> ((total - 1 - idx) * 8));
        }
        uint32_t w[80];
        for (int i = 0; i < 16; ++i)
            w[i] = (uint32_t(blk[i*4]) << 24) | (uint32_t(blk[i*4+1]) << 16) |
                   (uint32_t(blk[i*4+2]) << 8) | uint32_t(blk[i*4+3]);
        for (int i = 16; i < 80; ++i)
            w[i] = rol(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);

        uint32_t a=h[0], b=h[1], c=h[2], d=h[3], e=h[4];
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20)      { f = (b & c) | ((~b) & d);          k = 0x5A827999u; }
            else if (i < 40) { f = b ^ c ^ d;                     k = 0x6ED9EBA1u; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d);   k = 0x8F1BBCDCu; }
            else             { f = b ^ c ^ d;                     k = 0xCA62C1D6u; }
            const uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol(b, 30); b = a; a = t;
        }
        h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e;
    }
    for (int i = 0; i < 5; ++i) {
        out[i*4]   = uint8_t(h[i] >> 24); out[i*4+1] = uint8_t(h[i] >> 16);
        out[i*4+2] = uint8_t(h[i] >> 8);  out[i*4+3] = uint8_t(h[i]);
    }
}

void addrFromPub(const uint8_t pubComp[voile::kPubComp], uint8_t out[4]) {
    // Хеш от ключа, первые четыре байта. Через тот же HKDF, что и всё остальное, —
    // чтобы не заводить отдельную зависимость ради одной операции.
    uint8_t h[32];
    static const char kInfo[] = "voile addr";
    voile::hkdf(pubComp, voile::kPubComp, nullptr, 0,
                reinterpret_cast<const uint8_t*>(kInfo), sizeof(kInfo) - 1, h, sizeof(h));
    memcpy(out, h, 4);
}

size_t normalizePhrase(const char* in, char* out, size_t outLen) {
    if (!in || !out || outLen == 0) return 0;
    size_t n = 0;
    bool pendingSpace = false;
    bool started = false;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(in); *p; ++p) {
        unsigned char c = *p;
        // Схлопываем любые пробельные подряд в один, как делает телефонная версия.
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            if (started) pendingSpace = true;
            continue;
        }
        if (pendingSpace && n + 1 < outLen) { out[n++] = ' '; }
        pendingSpace = false;
        started = true;
        // Нижний регистр для латиницы. Кириллица в UTF-8 двухбайтовая, и понижение
        // регистра для неё требует разбора — делаем его отдельно ниже.
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + 32);
        else if (c == 0xD0 && p[1] >= 0x90 && p[1] <= 0x9F) {
            // А..П  ->  а..п  (0xD090..0xD09F -> 0xD0B0..0xD0BF)
            if (n + 2 < outLen) { out[n++] = (char)0xD0; out[n++] = (char)(p[1] + 0x20); }
            ++p; continue;
        }
        else if (c == 0xD0 && p[1] >= 0xA0 && p[1] <= 0xAF) {
            // Р..Я  ->  р..я  (0xD0A0..0xD0AF -> 0xD180..0xD18F)
            if (n + 2 < outLen) { out[n++] = (char)0xD1; out[n++] = (char)(p[1] - 0x20); }
            ++p; continue;
        }
        else if (c == 0xD0 && p[1] == 0x81) {
            // Ё -> ё
            if (n + 2 < outLen) { out[n++] = (char)0xD1; out[n++] = (char)0x91; }
            ++p; continue;
        }
        if (n + 1 < outLen) out[n++] = (char)c;
    }
    out[n] = 0;
    return n;
}

void deriveRendezvous(const char* phrase, Rendezvous& out) {
    char norm[128];
    const size_t nl = normalizePhrase(phrase, norm, sizeof(norm));

    // ── комната для ТЕЛЕФОНА ──
    // Формула телефонной версии (Rooms.infoHash): SHA-1 от "vual1-p:" + нормализованная
    // фраза. Менять нельзя — иначе плата не встретится ни с одной установленной версией.
    char roomInput[160];
    const int rl = snprintf(roomInput, sizeof(roomInput), "vual1-p:%s", norm);
    sha1(reinterpret_cast<const uint8_t*>(roomInput), size_t(rl > 0 ? rl : 0), out.phoneRoom);

    // ── адрес встречи для РАДИО ──
    // Своя формула: в эфире нужен короткий адрес, двадцать байт в заголовок не влезут.
    // Растяжение долгое — фразы люди придумывают короткие, и без него перебор по словарю
    // занял бы секунды.
    uint8_t acc[32] = {};
    static const char kStretch[] = "voile phrase";
    voile::hkdf(reinterpret_cast<const uint8_t*>(norm), nl, nullptr, 0,
                reinterpret_cast<const uint8_t*>(kStretch), sizeof(kStretch) - 1,
                acc, sizeof(acc));
    for (uint32_t i = 0; i < kPhraseRounds; ++i) {
        uint8_t next[32];
        voile::hkdf(acc, sizeof(acc), nullptr, 0,
                    reinterpret_cast<const uint8_t*>(kStretch), sizeof(kStretch) - 1,
                    next, sizeof(next));
        memcpy(acc, next, sizeof(acc));
    }
    // Адрес встречи и ключ обёртки — РАЗНЫМИ метками из одного корня: адрес виден в эфире,
    // и если бы ключ выводился из него же, подслушивающий получил бы оба.
    static const char kMeet[] = "voile meet";
    static const char kWrap[] = "voile wrap";
    uint8_t meet[32];
    voile::hkdf(acc, sizeof(acc), nullptr, 0,
                reinterpret_cast<const uint8_t*>(kMeet), sizeof(kMeet) - 1, meet, sizeof(meet));
    memcpy(out.meetAddr, meet, 4);
    voile::hkdf(acc, sizeof(acc), nullptr, 0,
                reinterpret_cast<const uint8_t*>(kWrap), sizeof(kWrap) - 1,
                out.wrapKey, sizeof(out.wrapKey));
}

void begin() { g_count = 0; memset(g_list, 0, sizeof(g_list)); }
size_t count() { return g_count; }

Contact* at(size_t i) { return (i < g_count && g_list[i].used) ? &g_list[i] : nullptr; }

Contact* byAddr(const uint8_t addr[4]) {
    for (size_t i = 0; i < kMaxContacts; ++i)
        if (g_list[i].used && memcmp(g_list[i].addr, addr, 4) == 0) return &g_list[i];
    return nullptr;
}

Contact* upsert(const char* name, const uint8_t pubComp[voile::kPubComp]) {
    uint8_t addr[4];
    addrFromPub(pubComp, addr);
    if (Contact* c = byAddr(addr)) {
        // Знакомый контакт: обновляем имя, ключ не трогаем. Смена ключа означала бы
        // подмену собеседника, и молча принимать её нельзя.
        if (name) snprintf(c->name, kNameLen, "%s", name);
        return c;
    }
    for (size_t i = 0; i < kMaxContacts; ++i) {
        if (g_list[i].used) continue;
        Contact& c = g_list[i];
        memset(&c, 0, sizeof(c));
        if (name) snprintf(c.name, kNameLen, "%s", name);
        memcpy(c.pubComp, pubComp, voile::kPubComp);
        memcpy(c.addr, addr, 4);
        c.used = true;
        if (i + 1 > g_count) g_count = i + 1;
        return &c;
    }
    return nullptr;
}

void markSeen(const uint8_t addr[4], bool wifi, uint32_t nowMs) {
    if (Contact* c = byAddr(addr)) {
        if (wifi) c->viaWifi = true; else c->viaLora = true;
        c->lastSeenMs = nowMs;
    }
}

bool removeAt(size_t i) {
    if (i >= g_count || !g_list[i].used) return false;
    // Сдвигаем хвост, а не просто гасим used: список показывается подряд по номерам,
    // и дырка в середине выглядела бы как пропавший без причины собеседник.
    for (size_t k = i; k + 1 < g_count; ++k) g_list[k] = g_list[k + 1];
    memset(&g_list[g_count - 1], 0, sizeof(Contact));
    --g_count;
    return true;
}

}  // namespace contacts

// ── своя личность ──────────────────────────────────────────────────────────────────────

namespace contacts {

namespace {
char           g_myName[kNameLen] = {};
voile::KeyPair g_myKeys{};
bool           g_haveKeys = false;
}  // namespace

const char* myName() { return g_myName; }
const uint8_t* myPub() { return g_myKeys.pubComp; }

void myAddr(uint8_t out[4]) {
    if (g_haveKeys) addrFromPub(g_myKeys.pubComp, out);
    else memset(out, 0, 4);
}

bool haveIdentity() { return g_haveKeys && g_myName[0] != 0; }

// Доступ к самим полям для сохранения на карту. Работу с картой держим ОТДЕЛЬНО:
// этот файл проверяется на компьютере, где ни карты, ни Arduino нет вовсе.
uint8_t* myPrivMutable() { return g_myKeys.priv; }
uint8_t* myPubMutable() { return g_myKeys.pubComp; }
char* myNameMutable() { return g_myName; }
void markKeysReady() { g_haveKeys = true; }

bool setMyName(const char* name) {
    if (name) snprintf(g_myName, sizeof(g_myName), "%s", name);
    if (g_haveKeys) return true;

    // Ключ создаём ОДИН раз и больше никогда: он и есть личность. Пересоздание разорвало
    // бы все прежние знакомства — собеседники узнают именно ключ, а не имя.
    g_haveKeys = voile::genKeyPair(g_myKeys);
    return g_haveKeys;
}

}  // namespace contacts
