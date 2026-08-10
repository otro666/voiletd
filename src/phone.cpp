#include "phone.h"

#include "board.h"
#include "cloak.h"
#include "contacts.h"
#include "net.h"
#include "store_sd.h"
#include "voile_crypto.h"

#include <Arduino.h>
#include <WiFi.h>

#include <lwip/sockets.h>
#include <lwip/inet.h>
#include <fcntl.h>
#include <string.h>
#include <stdio.h>

#include <mbedtls/base64.h>
#include <mbedtls/ecdsa.h>
#include <mbedtls/ecp.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/gcm.h>
#include <mbedtls/md.h>
#include <esp_random.h>
#include <esp_netif.h>
#include <lwip/ip6_addr.h>

namespace phone {

namespace {

// ── типы пакетов, дословно из телефонной версии ────────────────────────────────────────
//
// VoileDht.kt: магия 'V', версия, тип, номер обращения, идентификатор отправителя.
constexpr uint8_t kDhtMagic = 0x56;
constexpr uint8_t kDhtVer   = 1;
constexpr uint8_t D_STORE = 5, D_STORED = 6, D_FIND_VALUE = 7, D_VALUE = 8;

// VoileLink.kt
constexpr uint8_t L_PING = 1, L_PONG = 2, L_DATA = 3, L_ACK = 4;

constexpr size_t kIdLen = 20;

/** Заголовок X.509 для несжатой точки P-256 — телефон шлёт эфемерный ключ именно так
 *  (KeyPair.public.encoded даёт SubjectPublicKeyInfo). Префикс постоянный, 26 байт. */
const uint8_t kSpkiHead[26] = {
    0x30, 0x59, 0x30, 0x13, 0x06, 0x07, 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x02, 0x01,
    0x06, 0x08, 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x03, 0x01, 0x07, 0x03, 0x42, 0x00
};

// ── мелкие помощники ──────────────────────────────────────────────────────────────────

size_t b64(const uint8_t* in, size_t len, char* out, size_t cap) {
    size_t n = 0;
    if (mbedtls_base64_encode(reinterpret_cast<unsigned char*>(out), cap, &n, in, len) != 0)
        return 0;
    out[n] = 0;
    return n;
}

/** base64url без выравнивания — в визитке телефон использует именно его (Bytes.b64u). */
size_t b64u(const uint8_t* in, size_t len, char* out, size_t cap) {
    size_t n = b64(in, len, out, cap);
    if (!n) return 0;
    while (n && out[n - 1] == '=') out[--n] = 0;
    for (size_t i = 0; i < n; ++i) {
        if (out[i] == '+') out[i] = '-';
        else if (out[i] == '/') out[i] = '_';
    }
    return n;
}

size_t unb64u(const char* in, uint8_t* out, size_t cap) {
    char tmp[512];
    size_t n = strlen(in);
    if (n + 4 >= sizeof(tmp)) return 0;
    for (size_t i = 0; i < n; ++i) {
        char c = in[i];
        tmp[i] = (c == '-') ? '+' : (c == '_' ? '/' : c);
    }
    while (n % 4) tmp[n++] = '=';
    tmp[n] = 0;
    size_t got = 0;
    if (mbedtls_base64_decode(out, cap, &got,
                              reinterpret_cast<const unsigned char*>(tmp), n) != 0) return 0;
    return got;
}

/**
 * Хеш через общий интерфейс mbedtls_md, а не прямыми вызовами вроде mbedtls_sha1_starts:
 * у прямых функций имена и наличие суффикса _ret разнятся между версиями библиотеки, и
 * код, собравшийся у меня, у вас бы не собрался. Общий интерфейс есть везде.
 */
void mdHash(mbedtls_md_type_t type, const uint8_t* data, size_t len, uint8_t* out) {
    const mbedtls_md_info_t* info = mbedtls_md_info_from_type(type);
    if (info) mbedtls_md(info, data, len, out);
}

void sha1of(const uint8_t* data, size_t len, uint8_t out[20]) {
    mdHash(MBEDTLS_MD_SHA1, data, len, out);
}

/**
 * Значение поля из плоского JSON. Свой разбор, а не библиотека: полей мало, они известны
 * заранее, а тянуть парсер ради восьми ключей — лишние килобайты на плате, где их мало.
 * Вложенности в визитке нет, кроме массива строк, который разбирается отдельно.
 */
bool jsonStr(const char* json, const char* key, char* out, size_t cap) {
    char pat[24];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char* p = strstr(json, pat);
    if (!p) return false;
    p = strchr(p + strlen(pat), ':');
    if (!p) return false;
    ++p;
    while (*p == ' ') ++p;
    if (*p != '"') return false;
    ++p;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < cap) {
        if (*p == '\\' && p[1]) ++p;      // экранирование внутри строки
        out[i++] = *p++;
    }
    out[i] = 0;
    return *p == '"';
}

/** n-я строка из массива строк ("eps":["1.2.3.4:5",...]). false — элемента нет. */
bool jsonArrAt(const char* json, const char* key, size_t idx, char* out, size_t cap) {
    char pat[24];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char* p = strstr(json, pat);
    if (!p) return false;
    p = strchr(p + strlen(pat), '[');
    if (!p) return false;
    // Конец массива ищем, ПРОПУСКАЯ строки: у телефона в кандидатах бывают адреса IPv6
    // вида "[fe80::1]:5000", и наивный поиск ']' обрывал список на первом же таком —
    // подпись визитки после этого не сходилась, а выглядело как «телефон не отвечает».
    const char* end = nullptr;
    for (const char* q = p + 1; *q; ++q) {
        if (*q == '"') {                       // пропускаем строку целиком
            ++q;
            while (*q && *q != '"') { if (*q == '\\' && q[1]) ++q; ++q; }
            if (!*q) return false;
            continue;
        }
        if (*q == ']') { end = q; break; }
    }
    if (!end) return false;
    size_t seen = 0;
    while (p < end) {
        p = strchr(p + 1, '"');
        if (!p || p > end) return false;
        ++p;
        const char* q = strchr(p, '"');
        if (!q || q > end) return false;
        if (seen == idx) {
            size_t n = size_t(q - p);
            if (n + 1 > cap) n = cap - 1;
            memcpy(out, p, n);
            out[n] = 0;
            return true;
        }
        ++seen;
        p = q;
    }
    return false;
}

// ── состояние ─────────────────────────────────────────────────────────────────────────

constexpr char kVersion[] = "voile/1";
constexpr size_t kMaxCard = 1400;

bool     g_ready = false;
uint8_t  g_roomD[kIdLen] = {};        // комната встречи SHA-1("vual1-d:"+фраза)
bool     g_haveRoom = false;

voile::KeyPair g_eph;                  // эфемерная пара на эту сессию
uint8_t  g_nonce[16] = {};
uint8_t  g_oid[kIdLen] = {};           // идентификатор нашего предложения
uint8_t  g_pid[kIdLen] = {};           // наш «peer id» для рельсовых сообщений
char     g_answeredOid[40] = {};       // на этот оффер мы уже ответили
char     g_myCard[kMaxCard] = {};      // своя визитка, JSON
char     g_myX[64] = {}, g_myY[64] = {};

bool     g_linked = false;
char     g_peer[48] = {};
uint8_t  g_sendKey[32] = {}, g_recvKey[32] = {};
uint8_t  g_sendNonce[12] = {}, g_recvNonce[12] = {};
uint8_t  g_cloakKey[32] = {};
uint64_t g_outSeq = 0;
uint32_t g_inMsg = 0;
uint32_t g_dataSeq = 0;                // номер кадра данных линка

/**
 * Сокет сессии — СИСТЕМНЫЙ, двухсемейный, а не Arduino-обёртка.
 *
 * Обёртка WiFiUDP умеет только IPv4. С ней объявленный в визитке адрес IPv6 был
 * декорацией: телефон, предпочитающий IPv6, стучался в него — а там никто не слушал.
 * Один сокет семейства IPv6 с выключенным ограничением «только v6» принимает оба
 * семейства сразу; адреса IPv4 внутри него живут в отображённом виде ::ffff:а.б.в.г.
 */
int      g_fd = -1;
uint16_t g_sockPort = 0;
sockaddr_in6 g_peerSa = {};
bool     g_havePeer = false;
bool     g_pathUp = false;

/** Адрес IPv4 (сетевой порядок) и порт — в отображённый адрес двухсемейного сокета. */
void v4mapped(uint32_t ipNet, uint16_t port, sockaddr_in6& sa) {
    memset(&sa, 0, sizeof(sa));
    sa.sin6_family = AF_INET6;
    sa.sin6_port = htons(port);
    sa.sin6_addr.un.u32_addr[2] = htonl(0xFFFF);
    sa.sin6_addr.un.u32_addr[3] = ipNet;
}

/** Отправить датаграмму по заранее собранному адресу. */
bool sockSend(const sockaddr_in6& to, const uint8_t* data, size_t len) {
    if (g_fd < 0) return false;
    return sendto(g_fd, data, len, 0,
                  reinterpret_cast<const sockaddr*>(&to), sizeof(to)) == int(len);
}

uint32_t g_lastOffer = 0, g_lastPoll = 0, g_lastPing = 0;
void (*g_onText)(const char*, const char*) = nullptr;

/** Сборка визитки из частей комнаты: телефон режет длинные значения (DhtRail). */
char     g_asm[kMaxCard] = {};
char     g_asmId[16] = {};
uint8_t  g_asmHave = 0, g_asmTotal = 0;
char     g_asmPart[6][400] = {};

// ── криптография визитки ──────────────────────────────────────────────────────────────

mbedtls_ctr_drbg_context g_drbg;
mbedtls_entropy_context  g_entropy;

/**
 * Развернуть сжатую точку в координаты X и Y — телефон хранит личность парой чисел (JWK).
 *
 * Через запись точки в НЕСЖАТОМ виде, а не через внутренние поля структуры: публичный
 * путь не зависит от версии библиотеки, а разбор 65 байт тривиален — метка 0x04, затем
 * две координаты по 32 байта.
 */
/**
 * Разжать точку арифметикой, без помощи библиотеки.
 *
 * mbedTLS ветки 2 читать СЖАТЫЕ точки не умеет вовсе — возвращает «возможность
 * недоступна». А личность на плате хранится именно сжатой, и телефону нужны обе
 * координаты. Считаем сами: y² = x³ − 3x + b по модулю p, затем корень возведением в
 * степень (p+1)/4 — это работает, потому что у P-256 остаток p по модулю 4 равен трём.
 * Знак выбираем по первому байту, как принято: 0x02 — чётный y, 0x03 — нечётный.
 */
bool decompressManual(const uint8_t comp[33], uint8_t x[32], uint8_t y[32]) {
    if (comp[0] != 0x02 && comp[0] != 0x03) return false;

    mbedtls_ecp_group grp;
    mbedtls_mpi X, Y, T, E;
    mbedtls_ecp_group_init(&grp);
    mbedtls_mpi_init(&X); mbedtls_mpi_init(&Y);
    mbedtls_mpi_init(&T); mbedtls_mpi_init(&E);
    bool ok = false;
    do {
        if (mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1) != 0) break;
        if (mbedtls_mpi_read_binary(&X, comp + 1, 32) != 0) break;

        // T = x³ − 3x + b  (mod p)
        if (mbedtls_mpi_mul_mpi(&T, &X, &X) != 0) break;
        if (mbedtls_mpi_mod_mpi(&T, &T, &grp.P) != 0) break;
        if (mbedtls_mpi_mul_mpi(&T, &T, &X) != 0) break;
        if (mbedtls_mpi_mod_mpi(&T, &T, &grp.P) != 0) break;
        if (mbedtls_mpi_mul_int(&Y, &X, 3) != 0) break;
        if (mbedtls_mpi_sub_mpi(&T, &T, &Y) != 0) break;
        if (mbedtls_mpi_add_mpi(&T, &T, &grp.B) != 0) break;
        if (mbedtls_mpi_mod_mpi(&T, &T, &grp.P) != 0) break;

        // Y = T^((p+1)/4) mod p
        if (mbedtls_mpi_add_int(&E, &grp.P, 1) != 0) break;
        if (mbedtls_mpi_shift_r(&E, 2) != 0) break;
        if (mbedtls_mpi_exp_mod(&Y, &T, &E, &grp.P, nullptr) != 0) break;

        // Проверяем, что корень настоящий: иначе точка не на кривой.
        mbedtls_mpi chk;
        mbedtls_mpi_init(&chk);
        bool onCurve = false;
        if (mbedtls_mpi_mul_mpi(&chk, &Y, &Y) == 0 &&
            mbedtls_mpi_mod_mpi(&chk, &chk, &grp.P) == 0) {
            onCurve = mbedtls_mpi_cmp_mpi(&chk, &T) == 0;
        }
        mbedtls_mpi_free(&chk);
        if (!onCurve) break;

        if ((mbedtls_mpi_get_bit(&Y, 0) != 0) != ((comp[0] & 1) != 0)) {
            if (mbedtls_mpi_sub_mpi(&Y, &grp.P, &Y) != 0) break;
        }

        if (mbedtls_mpi_write_binary(&X, x, 32) != 0) break;
        if (mbedtls_mpi_write_binary(&Y, y, 32) != 0) break;
        ok = true;
    } while (false);
    mbedtls_mpi_free(&E); mbedtls_mpi_free(&T);
    mbedtls_mpi_free(&Y); mbedtls_mpi_free(&X);
    mbedtls_ecp_group_free(&grp);
    return ok;
}

bool uncompress(const uint8_t comp[33], uint8_t x[32], uint8_t y[32]) {
    mbedtls_ecp_group grp;
    mbedtls_ecp_point pt;
    mbedtls_ecp_group_init(&grp);
    mbedtls_ecp_point_init(&pt);
    bool ok = false;
    do {
        if (mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1) != 0) break;
        // Ветка 2 сжатые точки не читает: тогда считаем координаты сами.
        if (mbedtls_ecp_point_read_binary(&grp, &pt, comp, 33) != 0) {
            mbedtls_ecp_point_free(&pt);
            mbedtls_ecp_group_free(&grp);
            return decompressManual(comp, x, y);
        }
        uint8_t raw[65];
        size_t olen = 0;
        if (mbedtls_ecp_point_write_binary(&grp, &pt, MBEDTLS_ECP_PF_UNCOMPRESSED,
                                           &olen, raw, sizeof(raw)) != 0) break;
        if (olen != 65 || raw[0] != 0x04) break;
        memcpy(x, raw + 1, 32);
        memcpy(y, raw + 33, 32);
        ok = true;
    } while (false);
    mbedtls_ecp_point_free(&pt);
    mbedtls_ecp_group_free(&grp);
    return ok;
}

/** Подписать транскрипт визитки личным ключом (ECDSA/SHA-256, подпись в DER). */
size_t signTranscript(const char* t, size_t tlen, uint8_t* sig, size_t cap) {
    // Подпись P-256 в DER — до 72 байт. Проверяем сами: функция записи в этой версии
    // библиотеки ёмкость не принимает и переполнение не заметит.
    if (cap < 72) return 0;
    uint8_t h[32];
    mdHash(MBEDTLS_MD_SHA256, reinterpret_cast<const uint8_t*>(t), tlen, h);

    mbedtls_ecp_group grp;
    mbedtls_mpi d;
    mbedtls_ecp_group_init(&grp);
    mbedtls_mpi_init(&d);
    size_t n = 0;
    do {
        if (mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1) != 0) break;
        if (mbedtls_mpi_read_binary(&d, contacts::myPrivMutable(), 32) != 0) break;
        mbedtls_ecdsa_context ctx;
        mbedtls_ecdsa_init(&ctx);
        if (mbedtls_ecp_group_copy(&ctx.grp, &grp) == 0 &&
            mbedtls_mpi_copy(&ctx.d, &d) == 0) {
            // Ёмкости буфера в этой версии подписи нет: она пишет сколько нужно, а
            // максимум для P-256 в DER — 72 байта, под них и выделен sig у вызывающего.
            mbedtls_ecdsa_write_signature(&ctx, MBEDTLS_MD_SHA256, h, sizeof(h),
                                          sig, &n,
                                          mbedtls_ctr_drbg_random, &g_drbg);
        }
        mbedtls_ecdsa_free(&ctx);
    } while (false);
    mbedtls_mpi_free(&d);
    mbedtls_ecp_group_free(&grp);
    return n;
}

/** Проверить подпись визитки телефона по его координатам X и Y. */
bool verifyTranscript(const char* t, size_t tlen, const uint8_t x[32], const uint8_t y[32],
                      const uint8_t* sig, size_t siglen) {
    uint8_t h[32];
    mdHash(MBEDTLS_MD_SHA256, reinterpret_cast<const uint8_t*>(t), tlen, h);

    uint8_t raw[65];
    raw[0] = 0x04;
    memcpy(raw + 1, x, 32);
    memcpy(raw + 33, y, 32);

    mbedtls_ecdsa_context ctx;
    mbedtls_ecdsa_init(&ctx);
    bool ok = false;
    do {
        if (mbedtls_ecp_group_load(&ctx.grp, MBEDTLS_ECP_DP_SECP256R1) != 0) break;
        if (mbedtls_ecp_point_read_binary(&ctx.grp,
                                          &ctx.Q, raw, sizeof(raw)) != 0) break;
        ok = mbedtls_ecdsa_read_signature(&ctx, h, sizeof(h), sig, siglen) == 0;
    } while (false);
    mbedtls_ecdsa_free(&ctx);
    return ok;
}

/**
 * Транскрипт визитки — то, что накрыто подписью.
 * Формат дословно из VoileCrypto.cardTranscript: v|x|y|b64u(eph)|b64u(nonce)|эп1,эп2,
 * Кандидаты идут ОТСОРТИРОВАННЫМИ; у нас он один, поэтому сортировать нечего.
 */
size_t cardTranscript(char* out, size_t cap, const char* x, const char* y,
                      const char* ephB64, const char* nonceB64,
                      const char eps[][48], size_t epCount) {
    int n = snprintf(out, cap, "%s|%s|%s|%s|%s|", kVersion, x, y, ephB64, nonceB64);
    if (n <= 0) return 0;
    for (size_t i = 0; i < epCount; ++i) {
        const int k = snprintf(out + n, cap - size_t(n), "%s,", eps[i]);
        if (k <= 0) return 0;
        n += k;
    }
    return size_t(n);
}

/** Собрать свою визитку. Возвращает false, если нет личности или не хватило места. */
bool buildCard() {
    // Каждый отказ называет СЕБЯ. Пять причин с одной общей строкой «не удалось» —
    // это ровно та слепота, из-за которой мы неделю гадали над радио.
    if (!contacts::haveIdentity()) { store::log("phone", "визитка: нет личности"); return false; }

    uint8_t x[32], y[32];
    if (!uncompress(contacts::myPub(), x, y)) {
        store::log("phone", "визитка: не разжалась точка личности");
        return false;
    }
    b64u(x, 32, g_myX, sizeof(g_myX));
    b64u(y, 32, g_myY, sizeof(g_myY));

    if (!voile::genKeyPair(g_eph)) {
        store::log("phone", "визитка: не вышел эфемерный ключ");
        return false;
    }
    esp_fill_random(g_nonce, sizeof(g_nonce));
    esp_fill_random(g_oid, sizeof(g_oid));
    esp_fill_random(g_pid, sizeof(g_pid));

    // Эфемерный ключ телефон ждёт в формате X.509 — постоянный заголовок плюс несжатая точка.
    uint8_t ex[32], ey[32];
    if (!uncompress(g_eph.pubComp, ex, ey)) {
        store::log("phone", "визитка: не разжался эфемерный ключ");
        return false;
    }
    uint8_t spki[26 + 65];
    memcpy(spki, kSpkiHead, sizeof(kSpkiHead));
    spki[26] = 0x04;
    memcpy(spki + 27, ex, 32);
    memcpy(spki + 59, ey, 32);

    char ephB64[160], nonceB64[32];
    b64u(spki, sizeof(spki), ephB64, sizeof(ephB64));
    b64u(g_nonce, sizeof(g_nonce), nonceB64, sizeof(nonceB64));

    // Кандидаты: по ним телефон к нам и постучится.
    //
    // IPv6 добавляем ПЕРВЫМ делом, если он есть: телефонная версия ходит по IPv6
    // предпочтительно (в её коде это прямо заявлено), и без такого кандидата встреча
    // в сетях, где IPv4 за общим адресом, просто не состоится.
    char eps[2][48];
    size_t epCount = 0;

    // Сперва глобальный адрес, при его отсутствии — локальный для звена: в домашней
    // сети собеседник всё равно за тем же маршрутизатором, и локального хватает.
    esp_ip6_addr_t ip6;
    esp_netif_t* sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    bool have6 = sta && esp_netif_get_ip6_global(sta, &ip6) == ESP_OK;
    if (!have6 && sta) have6 = esp_netif_get_ip6_linklocal(sta, &ip6) == ESP_OK;
    if (have6) {
        char a[46];
        ip6addr_ntoa_r(reinterpret_cast<const ip6_addr_t*>(&ip6), a, sizeof(a));
        snprintf(eps[epCount++], sizeof(eps[0]), "[%s]:%u", a, unsigned(g_sockPort));
    }

    const IPAddress ip = WiFi.localIP();
    snprintf(eps[epCount++], sizeof(eps[0]), "%u.%u.%u.%u:%u",
             ip[0], ip[1], ip[2], ip[3], unsigned(g_sockPort));

    // Подпись накрывает кандидатов ОТСОРТИРОВАННЫМИ — как у телефона (eps.sorted()).
    if (epCount == 2 && strcmp(eps[0], eps[1]) > 0) {
        char tmp[48];
        strcpy(tmp, eps[0]); strcpy(eps[0], eps[1]); strcpy(eps[1], tmp);
    }

    char t[768];
    const size_t tlen = cardTranscript(t, sizeof(t), g_myX, g_myY, ephB64, nonceB64,
                                       eps, epCount);
    if (!tlen) { store::log("phone", "визитка: транскрипт не собрался"); return false; }

    uint8_t sig[80];
    const size_t siglen = signTranscript(t, tlen, sig, sizeof(sig));
    if (!siglen) { store::log("phone", "визитка: подпись не вышла"); return false; }
    char sigB64[128];
    b64u(sig, siglen, sigB64, sizeof(sigB64));

    char epsJson[110];
    if (epCount == 2) snprintf(epsJson, sizeof(epsJson), "\"%s\",\"%s\"", eps[0], eps[1]);
    else              snprintf(epsJson, sizeof(epsJson), "\"%s\"", eps[0]);

    const int n = snprintf(g_myCard, sizeof(g_myCard),
        "{\"v\":\"%s\",\"x\":\"%s\",\"y\":\"%s\",\"eph\":\"%s\",\"n\":\"%s\","
        "\"eps\":[%s],\"sig\":\"%s\"}",
        kVersion, g_myX, g_myY, ephB64, nonceB64, epsJson, sigB64);
    if (n <= 0 || size_t(n) >= sizeof(g_myCard)) {
        store::log("phone", "визитка: не поместилась");
        return false;
    }
    ets_printf("[vual] визитка готова, %d байт, кандидатов %u: %s\n",
               n, unsigned(epCount), eps[0]);
    return true;
}

// ── ключи сессии ──────────────────────────────────────────────────────────────────────

/**
 * Вывести ключи линка из визиток. Дословно VoileCrypto.derive.
 *
 * Мы ВСЕГДА инициатор: предложение в комнату кладём мы, телефон отвечает (см. RailManager —
 * ответивший считает себя отвечающим). Перепутать роли значит развести гаммы: сообщения
 * уходили бы в пустоту, и найти это было бы крайне тяжело.
 */
bool deriveKeys(const char* peerX, const char* peerY, const uint8_t* peerEphSpki,
                size_t peerEphLen, const char* peerNonceB64, bool iAmInitiator) {
    if (peerEphLen != 26 + 65 || peerEphSpki[26] != 0x04) return false;

    // Из X.509 обратно в сжатую точку — её ждёт наш ECDH.
    uint8_t comp[33];
    comp[0] = uint8_t(0x02 | (peerEphSpki[26 + 64] & 1));
    memcpy(comp + 1, peerEphSpki + 27, 32);

    uint8_t shared[32];
    if (!voile::ecdh(g_eph.priv, comp, shared)) return false;

    char myNonceB64[32];
    b64u(g_nonce, sizeof(g_nonce), myNonceB64, sizeof(myNonceB64));

    // Порядок в транскрипте задаёт РОЛЬ: первым идёт тот, кто прислал предложение.
    // Перепутать роли — значит развести гаммы: кадры уходят, но не читаются ни одной
    // стороной, и выглядит это как полная тишина.
    const char* aX = iAmInitiator ? g_myX : peerX;
    const char* aY = iAmInitiator ? g_myY : peerY;
    const char* bX = iAmInitiator ? peerX : g_myX;
    const char* bY = iAmInitiator ? peerY : g_myY;
    const char* aN = iAmInitiator ? myNonceB64 : peerNonceB64;
    const char* bN = iAmInitiator ? peerNonceB64 : myNonceB64;

    char tr[512];
    const int trn = snprintf(tr, sizeof(tr), "%s|%s%s|%s%s|%s|%s",
                             kVersion, aX, aY, bX, bY, aN, bN);
    if (trn <= 0) return false;

    uint8_t i2r[44], r2i[44];
    voile::hkdf(shared, sizeof(shared), reinterpret_cast<const uint8_t*>(tr), size_t(trn),
                reinterpret_cast<const uint8_t*>("voile i2r"), 9, i2r, sizeof(i2r));
    voile::hkdf(shared, sizeof(shared), reinterpret_cast<const uint8_t*>(tr), size_t(trn),
                reinterpret_cast<const uint8_t*>("voile r2i"), 9, r2i, sizeof(r2i));

    // Инициатор пишет ключом i2r и читает r2i, отвечающий — наоборот.
    const uint8_t* mineKm   = iAmInitiator ? i2r : r2i;
    const uint8_t* theirsKm = iAmInitiator ? r2i : i2r;
    memcpy(g_sendKey, mineKm, 32);       memcpy(g_sendNonce, mineKm + 32, 12);
    memcpy(g_recvKey, theirsKm, 32);     memcpy(g_recvNonce, theirsKm + 32, 12);

    // Ключ обфускации — из того же секрета, соль нулевая (VoileCrypto.cloakSeed).
    static const uint8_t zero[32] = {};
    voile::hkdf(shared, sizeof(shared), zero, sizeof(zero),
                reinterpret_cast<const uint8_t*>("voile cloak"), 11, g_cloakKey, 32);
    return true;
}

/** Одноразовый вектор: базовый ⊕ номер, начиная с четвёртого байта (FrameCipher.nonce). */
void nonceFor(const uint8_t base[12], uint64_t seq, uint8_t out[12]) {
    memcpy(out, base, 12);
    for (int i = 0; i < 8; ++i) out[4 + i] ^= uint8_t(seq >> (56 - 8 * i));
}

// ── кадры линка ───────────────────────────────────────────────────────────────────────

/** Отправить кадр линка: [тип][номер 8][шифртекст+тег], сверху обфускация. */
bool linkSend(uint8_t type, const uint8_t* body, size_t len) {
    if (!g_havePeer) return false;

    const uint64_t seq = g_outSeq++;
    uint8_t aad[9];
    aad[0] = type;
    for (int i = 0; i < 8; ++i) aad[1 + i] = uint8_t(seq >> (56 - 8 * i));

    uint8_t nonce[12];
    nonceFor(g_sendNonce, seq, nonce);

    static uint8_t pkt[1500];
    if (9 + len + 16 > sizeof(pkt)) return false;
    memcpy(pkt, aad, 9);

    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    bool ok = false;
    if (mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, g_sendKey, 256) == 0) {
        ok = mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, len,
                                       nonce, 12, aad, 9, body, pkt + 9,
                                       16, pkt + 9 + len) == 0;
    }
    mbedtls_gcm_free(&gcm);
    if (!ok) return false;

    static uint8_t sealed[1600];
    const size_t sn = cloak::seal(g_cloakKey, pkt, 9 + len + 16, sealed, sizeof(sealed));
    if (!sn) return false;

    return sockSend(g_peerSa, sealed, sn);
}

/** Разобрать принятый кадр линка. Возвращает тип или 0. */
uint8_t linkRecv(const uint8_t* buf, size_t len, uint8_t* body, size_t cap, size_t& bodyLen) {
    static uint8_t plain[1600];
    const size_t n = cloak::open(g_cloakKey, buf, len, plain, sizeof(plain));
    if (n < 9 + 16) return 0;

    const uint8_t type = plain[0];
    uint64_t seq = 0;
    for (int i = 0; i < 8; ++i) seq = (seq << 8) | plain[1 + i];

    const size_t ctLen = n - 9 - 16;
    if (ctLen > cap) return 0;

    uint8_t nonce[12];
    nonceFor(g_recvNonce, seq, nonce);

    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    bool ok = false;
    if (mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, g_recvKey, 256) == 0) {
        ok = mbedtls_gcm_auth_decrypt(&gcm, ctLen, nonce, 12, plain, 9,
                                      plain + 9 + ctLen, 16,
                                      plain + 9, body) == 0;
    }
    mbedtls_gcm_free(&gcm);
    if (!ok) return 0;
    bodyLen = ctLen;
    return type;
}

/** Подтверждение приёма: тело — номер следующего ожидаемого кадра (VoileLink.sendAck). */
void sendAck(uint32_t next) {
    uint8_t b[4] = {uint8_t(next >> 24), uint8_t(next >> 16), uint8_t(next >> 8), uint8_t(next)};
    linkSend(L_ACK, b, sizeof(b));
}

// ── комната встречи ───────────────────────────────────────────────────────────────────

/** Отправить пакет DHT телефону: [магия][версия][тип][обращение 4][наш id 20][тело]. */
bool dhtSend(const net::Neighbour* nb, uint8_t type, const uint8_t* body, size_t len) {
    if (!nb) return false;
    static uint8_t pkt[1400];
    if (7 + kIdLen + len > sizeof(pkt)) return false;
    const uint32_t tx = esp_random();
    pkt[0] = kDhtMagic; pkt[1] = kDhtVer; pkt[2] = type;
    pkt[3] = uint8_t(tx >> 24); pkt[4] = uint8_t(tx >> 16);
    pkt[5] = uint8_t(tx >> 8);  pkt[6] = uint8_t(tx);
    memcpy(pkt + 7, net::myId(), kIdLen);
    memcpy(pkt + 7 + kIdLen, body, len);

    // Ключ обфускации выводится из идентификатора ПОЛУЧАТЕЛЯ — так требует телефон.
    uint8_t key[32];
    cloak::keyForNode(nb->id, key);
    static uint8_t sealed[1500];
    const size_t sn = cloak::seal(key, pkt, 7 + kIdLen + len, sealed, sizeof(sealed));
    if (!sn) return false;

    sockaddr_in6 to;
    v4mapped(nb->ip, nb->port, to);
    return sockSend(to, sealed, sn);
}

/** Положить своё предложение в комнату (DHT STORE), как это делает DhtRail.publish. */
void publishOffer() {
    if (!g_haveRoom || !g_myCard[0]) return;

    char pidB64[40], oidB64[40];
    b64(g_pid, sizeof(g_pid), pidB64, sizeof(pidB64));
    b64(g_oid, sizeof(g_oid), oidB64, sizeof(oidB64));

    // Рельсовое сообщение: тип «o» — предложение, внутри наша визитка (Wire.encodeRail).
    static char wire[kMaxCard + 256];
    char ihHex[41];
    for (size_t i = 0; i < kIdLen; ++i) snprintf(ihHex + i * 2, 3, "%02x", g_roomD[i]);
    const int wn = snprintf(wire, sizeof(wire),
        "0|{\"v\":1,\"t\":\"o\",\"ih\":\"%s\",\"pid\":\"%s\",\"oid\":\"%s\",\"sdp\":%s}",
        ihHex, pidB64, oidB64, g_myCard);
    if (wn <= 0) return;

    // Значение DHT: ключ комнаты и следом тело.
    static uint8_t body[kMaxCard + 300];
    memcpy(body, g_roomD, kIdLen);
    size_t total = size_t(wn);
    if (kIdLen + total > sizeof(body)) return;
    memcpy(body + kIdLen, wire, total);

    size_t sent = 0;
    for (size_t i = 0; i < net::neighbourCount(); ++i) {
        if (dhtSend(net::neighbourAt(i), D_STORE, body, kIdLen + total)) ++sent;
    }
    if (sent) store::log("phone", "предложение положено в комнату");
}

/**
 * Ответить на чужое предложение: тот же рельсовый кадр, но тип «a» и идентификатор
 * ЧУЖОГО оффера — по нему отвечающая сторона узнаёт свой (Wire.RailWire("a", …)).
 */
void publishAnswer(const char* theirOid) {
    if (!g_haveRoom || !g_myCard[0]) return;

    char pidB64[40];
    b64(g_pid, sizeof(g_pid), pidB64, sizeof(pidB64));

    static char wire[kMaxCard + 256];
    char ihHex[41];
    for (size_t i = 0; i < kIdLen; ++i) snprintf(ihHex + i * 2, 3, "%02x", g_roomD[i]);
    const int wn = snprintf(wire, sizeof(wire),
        "0|{\"v\":1,\"t\":\"a\",\"ih\":\"%s\",\"pid\":\"%s\",\"oid\":\"%s\",\"sdp\":%s}",
        ihHex, pidB64, theirOid, g_myCard);
    if (wn <= 0) return;

    static uint8_t body[kMaxCard + 300];
    memcpy(body, g_roomD, kIdLen);
    if (kIdLen + size_t(wn) > sizeof(body)) return;
    memcpy(body + kIdLen, wire, size_t(wn));

    size_t sent = 0;
    for (size_t i = 0; i < net::neighbourCount(); ++i)
        if (dhtSend(net::neighbourAt(i), D_STORE, body, kIdLen + size_t(wn))) ++sent;
    if (sent) store::log("phone", "ответ положен в комнату");
}

/** Спросить комнату (DHT FIND_VALUE). */
void pollRoom() {
    if (!g_haveRoom) return;
    for (size_t i = 0; i < net::neighbourCount(); ++i) {
        dhtSend(net::neighbourAt(i), D_FIND_VALUE, g_roomD, kIdLen);
    }
}

/** Разобрать визитку телефона и поднять линк. */
/**
 * Принять визитку собеседника и поднять канал.
 *
 * iAmInitiator: true — это ОТВЕТ на наше предложение; false — это ЧУЖОЕ предложение,
 * на которое отвечаем мы. Роль решает порядок в транскрипте и раскладку ключей.
 */
bool acceptCard(const char* cardJson, bool iAmInitiator) {
    char x[64], y[64], ephB64[200], nonceB64[40], sigB64[160], ep[48];
    if (!jsonStr(cardJson, "x", x, sizeof(x))) return false;
    if (!jsonStr(cardJson, "y", y, sizeof(y))) return false;
    if (!jsonStr(cardJson, "eph", ephB64, sizeof(ephB64))) return false;
    if (!jsonStr(cardJson, "n", nonceB64, sizeof(nonceB64))) return false;
    if (!jsonStr(cardJson, "sig", sigB64, sizeof(sigB64))) return false;

    uint8_t xb[32], yb[32], eph[128], sig[96];
    if (unb64u(x, xb, sizeof(xb)) != 32) return false;
    if (unb64u(y, yb, sizeof(yb)) != 32) return false;
    const size_t ephLen = unb64u(ephB64, eph, sizeof(eph));
    const size_t sigLen = unb64u(sigB64, sig, sizeof(sig));
    if (!ephLen || !sigLen) return false;

    // Проверяем подпись: транскрипт собирается из ТЕХ ЖЕ полей и в том же порядке,
    // включая отсортированный список кандидатов.
    char eps[6][48];
    size_t epCount = 0;
    while (epCount < 6 && jsonArrAt(cardJson, "eps", epCount, eps[epCount], sizeof(eps[0])))
        ++epCount;
    // Сортировка строк — как в телефонной версии (eps.sorted()).
    for (size_t a = 0; a + 1 < epCount; ++a)
        for (size_t b = a + 1; b < epCount; ++b)
            if (strcmp(eps[a], eps[b]) > 0) {
                char t[48];
                strcpy(t, eps[a]); strcpy(eps[a], eps[b]); strcpy(eps[b], t);
            }

    char tr[900];
    int o = snprintf(tr, sizeof(tr), "%s|%s|%s|%s|%s|", kVersion, x, y, ephB64, nonceB64);
    for (size_t i = 0; i < epCount && o > 0; ++i)
        o += snprintf(tr + o, sizeof(tr) - size_t(o), "%s,", eps[i]);
    if (o <= 0) return false;

    if (!verifyTranscript(tr, size_t(o), xb, yb, sig, sigLen)) {
        store::log("phone", "подпись визитки неверна");
        return false;
    }
    if (!deriveKeys(x, y, eph, ephLen, nonceB64, iAmInitiator)) return false;

    // Куда стучаться: первый кандидат из визитки.
    if (!epCount) return false;
    // Ищем первый кандидат, который мы умеем набрать. IPv6 телефон предлагает первым,
    // но плата ходит по нему не всегда — тогда берём следующий, а не сдаёмся.
    bool dialled = false;
    for (size_t i = 0; i < epCount && !dialled; ++i) {
        strncpy(ep, eps[i], sizeof(ep) - 1);
        ep[sizeof(ep) - 1] = 0;
        char* colon = strrchr(ep, ':');
        if (!colon) continue;
        *colon = 0;
        const uint16_t port = uint16_t(atoi(colon + 1));

        if (ep[0] == '[') {
            // Адрес IPv6 в скобках — собираем sockaddr семейства v6 напрямую.
            char* addr = ep + 1;
            char* close = strchr(addr, ']');
            if (!close) continue;
            *close = 0;
            ip6_addr_t v6;
            if (!ip6addr_aton(addr, &v6)) continue;
            memset(&g_peerSa, 0, sizeof(g_peerSa));
            g_peerSa.sin6_family = AF_INET6;
            g_peerSa.sin6_port = htons(port);
            memcpy(&g_peerSa.sin6_addr, &v6, sizeof(v6));
            dialled = true;
        } else {
            // Адрес IPv4 — в отображённый вид, чтобы уйти тем же двухсемейным сокетом.
            IPAddress ip;
            if (!ip.fromString(ep)) continue;
            v4mapped(uint32_t(ip), port, g_peerSa);
            dialled = true;
        }
    }
    if (!dialled) {
        store::log("phone", "ни один адрес собеседника не разобран");
        return false;
    }
    g_havePeer = true;

    snprintf(g_peer, sizeof(g_peer), "%.*s", 16, x);   // короткий отпечаток для журнала
    g_linked = true;
    g_pathUp = false;
    g_outSeq = 0;
    g_dataSeq = 0;
    store::log("phone", iAmInitiator ? "ответ принят, поднимаю канал"
                                     : "предложение принято, отвечаю");
    linkSend(L_PING, nullptr, 0);
    return true;
}

/** Разбор значения комнаты: целое «0|…» или часть «1|id|номер|всего|…». */
void onRoomValue(const char* v, size_t len) {
    if (len < 2) return;
    static char whole[kMaxCard + 256];

    if (v[0] == '0' && v[1] == '|') {
        if (len - 2 >= sizeof(whole)) return;
        memcpy(whole, v + 2, len - 2);
        whole[len - 2] = 0;
    } else if (v[0] == '1' && v[1] == '|') {
        // 1|id|номер|всего|данные
        char id[16]; int idx = 0, total = 0;
        const char* p = v + 2;
        const char* q = strchr(p, '|'); if (!q) return;
        size_t n = size_t(q - p); if (n >= sizeof(id)) return;
        memcpy(id, p, n); id[n] = 0;
        p = q + 1; idx = atoi(p);
        q = strchr(p, '|'); if (!q) return;
        p = q + 1; total = atoi(p);
        q = strchr(p, '|'); if (!q) return;
        p = q + 1;
        if (total <= 0 || total > 6 || idx < 0 || idx >= total) return;

        if (strcmp(g_asmId, id) != 0) {
            snprintf(g_asmId, sizeof(g_asmId), "%s", id);
            g_asmHave = 0; g_asmTotal = uint8_t(total);
            for (int i = 0; i < 6; ++i) g_asmPart[i][0] = 0;
        }
        const size_t plen = len - size_t(p - v);
        if (plen >= sizeof(g_asmPart[0])) return;
        if (!g_asmPart[idx][0]) ++g_asmHave;
        memcpy(g_asmPart[idx], p, plen);
        g_asmPart[idx][plen] = 0;
        if (g_asmHave < g_asmTotal) return;

        whole[0] = 0;
        for (int i = 0; i < g_asmTotal; ++i) strncat(whole, g_asmPart[i], sizeof(whole) - strlen(whole) - 1);
        g_asmId[0] = 0; g_asmHave = 0;
    } else {
        return;
    }

    if (g_linked) return;                         // канал уже есть — чужое не трогаем

    char t[4], oid[40], pid[40];
    if (!jsonStr(whole, "t", t, sizeof(t))) return;
    if (!jsonStr(whole, "oid", oid, sizeof(oid))) return;

    // Своё же сообщение прилетает обратно тем же опросом комнаты — узнаём по подписи
    // и молча пропускаем, иначе плата попыталась бы знакомиться сама с собой.
    char myPid[40];
    b64(g_pid, sizeof(g_pid), myPid, sizeof(myPid));
    if (jsonStr(whole, "pid", pid, sizeof(pid)) && strcmp(pid, myPid) == 0) return;

    const char* sdp = strstr(whole, "\"sdp\":");
    if (!sdp) return;
    sdp += 6;
    if (*sdp != '{') return;                      // визитка — вложенный объект

    if (t[0] == 'a') {
        // Ответ на НАШЕ предложение: сверяем, что отвечают именно нам.
        char myOid[40];
        b64(g_oid, sizeof(g_oid), myOid, sizeof(myOid));
        if (strcmp(oid, myOid) != 0) return;
        acceptCard(sdp, /*iAmInitiator=*/true);
        return;
    }

    if (t[0] == 'o') {
        // ЧУЖОЕ предложение — отвечаем на него, как это делает телефон. Раньше плата
        // только клала своё и ждала: если телефон в это время делал ровно то же самое,
        // обе стороны ждали друг друга и встреча не складывалась никогда.
        //
        // На один и тот же оффер отвечаем один раз: копии приезжают по каждой рельсе.
        if (strcmp(oid, g_answeredOid) == 0) return;
        snprintf(g_answeredOid, sizeof(g_answeredOid), "%s", oid);

        if (!acceptCard(sdp, /*iAmInitiator=*/false)) {
            g_answeredOid[0] = 0;      // не сложилось — следующая копия попробует снова
            return;
        }
        publishAnswer(oid);
    }
}

/** Приём на сокете сессии: ответы DHT и кадры линка. Сокет неблокирующий. */
void readSocket() {
    if (g_fd < 0) return;
    for (;;) {
        static uint8_t buf[1600];
        sockaddr_in6 from = {};
        socklen_t fromLen = sizeof(from);
        const int n = recvfrom(g_fd, buf, sizeof(buf), 0,
                               reinterpret_cast<sockaddr*>(&from), &fromLen);
        if (n <= 0) return;                       // пусто — до следующего круга

        // Сперва линк: если он поднят, большая часть пакетов именно его.
        if (g_linked) {
            static uint8_t body[1400];
            size_t bodyLen = 0;
            const uint8_t type = linkRecv(buf, size_t(n), body, sizeof(body), bodyLen);
            if (type) {
                if (!g_pathUp) {
                    // Отвечаем туда, ОТКУДА реально пришло: телефон может выйти не с
                    // того адреса, что объявлял, — путь важнее визитки.
                    g_pathUp = true;
                    g_peerSa = from;
                    store::log("phone", "канал с телефоном установлен");
                }
                if (type == L_PING) {
                    linkSend(L_PONG, nullptr, 0);
                } else if (type == L_DATA && bodyLen >= 8) {
                    const uint32_t seq = (uint32_t(body[0]) << 24) | (uint32_t(body[1]) << 16) |
                                         (uint32_t(body[2]) << 8) | body[3];
                    const uint32_t hdr = (uint32_t(body[4]) << 24) | (uint32_t(body[5]) << 16) |
                                         (uint32_t(body[6]) << 8) | body[7];
                    const bool last = (hdr & 1) != 0;
                    sendAck(seq + 1);
                    // Кадр приложения: первый байт 1 — текст. Куски длинных сообщений
                    // собирать не пытаемся: в переписке они помещаются в один.
                    if (last && bodyLen > 9 && body[8] == 1) {
                        static char text[512];
                        const size_t tl = bodyLen - 9 < sizeof(text) - 1 ? bodyLen - 9 : sizeof(text) - 1;
                        memcpy(text, body + 9, tl);
                        text[tl] = 0;
                        if (g_onText) g_onText(g_peer, text);
                    }
                    ++g_inMsg;
                }
                continue;
            }
        }

        // Иначе — ответ DHT нашим же ключом узла.
        uint8_t key[32];
        cloak::keyForNode(net::myId(), key);
        static uint8_t plain[1500];
        const size_t pn = cloak::open(key, buf, size_t(n), plain, sizeof(plain));
        if (pn >= 7 + kIdLen && plain[0] == kDhtMagic && plain[2] == D_VALUE) {
            const uint8_t* p = plain + 7 + kIdLen;
            size_t left = pn - 7 - kIdLen;
            while (left >= 2) {
                const size_t vlen = (size_t(p[0]) << 8) | p[1];
                if (vlen + 2 > left) break;
                onRoomValue(reinterpret_cast<const char*>(p + 2), vlen);
                p += 2 + vlen;
                left -= 2 + vlen;
            }
        }
    }
}

}  // namespace

// ── открытый интерфейс ────────────────────────────────────────────────────────────────

bool begin() {
    if (g_ready) return true;
    if (!contacts::haveIdentity()) return false;

    mbedtls_entropy_init(&g_entropy);
    mbedtls_ctr_drbg_init(&g_drbg);
    static const char kSeed[] = "vual phone";
    if (mbedtls_ctr_drbg_seed(&g_drbg, mbedtls_entropy_func, &g_entropy,
                              reinterpret_cast<const unsigned char*>(kSeed),
                              sizeof(kSeed) - 1) != 0) return false;

    // Свой порт: сессия телефона живёт отдельно от DHT — так же, как у него самого.
    g_sockPort = uint16_t(41000 + (esp_random() % 15000));

    // Один сокет семейства IPv6 с выключенным «только v6» слушает ОБА семейства.
    // Обёртка WiFiUDP этого не умеет — с ней объявленный адрес IPv6 был бы приманкой,
    // в которую телефон стучится впустую.
    g_fd = socket(AF_INET6, SOCK_DGRAM, 0);
    if (g_fd < 0) { store::log("phone", "сокет не открылся"); return false; }
    int off = 0;
    setsockopt(g_fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off));
    sockaddr_in6 bindSa = {};
    bindSa.sin6_family = AF_INET6;
    bindSa.sin6_port = htons(g_sockPort);
    if (bind(g_fd, reinterpret_cast<sockaddr*>(&bindSa), sizeof(bindSa)) != 0) {
        store::log("phone", "порт сессии занят");
        ::close(g_fd); g_fd = -1;
        return false;
    }
    fcntl(g_fd, F_SETFL, O_NONBLOCK);

    if (!buildCard()) { store::log("phone", "визитку собрать не удалось"); return false; }
    g_ready = true;
    store::log("phone", "готов к встрече с телефоном");
    return true;
}

void setPhrase(const char* phrase) {
    if (!phrase || !phrase[0]) { g_haveRoom = false; return; }
    char norm[128];
    const size_t nl = contacts::normalizePhrase(phrase, norm, sizeof(norm));
    char in[160];
    const int n = snprintf(in, sizeof(in), "vual1-d:%.*s", int(nl), norm);
    if (n <= 0) return;
    sha1of(reinterpret_cast<const uint8_t*>(in), size_t(n), g_roomD);
    g_haveRoom = true;
    g_lastOffer = 0;                 // объявиться сразу, не дожидаясь круга
    g_answeredOid[0] = 0;

    // Визитку пересобираем НА КАЖДОЕ знакомство, а не один раз при старте. Причины две.
    // Адрес IPv6 приходит через несколько секунд после подключения к сети — визитка,
    // собранная при старте, его не застала и навсегда объявляла один IPv4. И эфемерный
    // ключ обязан быть свежим на каждую встречу: в этом весь его смысл.
    if (g_ready && !buildCard()) store::log("phone", "визитка не пересобралась");
}

bool linked() { return g_linked && g_pathUp; }
const char* peerName() { return g_peer; }
void setOnText(void (*cb)(const char*, const char*)) { g_onText = cb; }

bool sendText(const char* text) {
    if (!linked() || !text || !text[0]) return false;
    const size_t tl = strlen(text);
    static uint8_t body[1200];
    if (9 + tl > sizeof(body)) return false;

    // Кадр данных линка: номер, заголовок (номер сообщения и признак конца), начинка.
    const uint32_t seq = g_dataSeq++;
    const uint32_t hdr = (uint32_t(seq) << 1) | 1;      // одно сообщение — сразу последнее
    body[0] = uint8_t(seq >> 24); body[1] = uint8_t(seq >> 16);
    body[2] = uint8_t(seq >> 8);  body[3] = uint8_t(seq);
    body[4] = uint8_t(hdr >> 24); body[5] = uint8_t(hdr >> 16);
    body[6] = uint8_t(hdr >> 8);  body[7] = uint8_t(hdr);
    body[8] = 1;                                        // 1 — текст, 0 — двоичные данные
    memcpy(body + 9, text, tl);
    return linkSend(L_DATA, body, 9 + tl);
}

void pump() {
    if (!g_ready) {
        // Пробуем подняться, когда появились и личность, и сеть — но НЕ каждый круг:
        // неудачная попытка каждые несколько миллисекунд забивала журнал одинаковыми
        // строками так, что в нём было не найти ничего другого.
        static uint32_t lastTry = 0;
        const uint32_t now = millis();
        if (now - lastTry < 5000) return;
        lastTry = now;
        if (net::ready() && contacts::haveIdentity()) begin();
        return;
    }

    readSocket();
    const uint32_t now = millis();

    if (!g_linked && g_haveRoom && net::neighbourCount() > 0) {
        // Предложение кладём раз в 10 с, комнату опрашиваем раз в 2 с: телефон отвечает
        // не мгновенно — ему нужно собрать свою визитку и опросить комнату у себя.
        if (now - g_lastOffer > 10000) { g_lastOffer = now; publishOffer(); }
        if (now - g_lastPoll > 2000)   { g_lastPoll = now;  pollRoom(); }
    }

    // Пока путь не установлен — стучимся; после установления шлём редкие PING,
    // иначе телефон посчитает нас пропавшими и закроет сессию.
    if (g_linked && now - g_lastPing > (g_pathUp ? 15000u : 1000u)) {
        g_lastPing = now;
        linkSend(L_PING, nullptr, 0);
    }
}

}  // namespace phone
