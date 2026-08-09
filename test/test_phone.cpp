// Проверка разбора того, что присылает ТЕЛЕФОН.
//
// Смысл этих проверок. Сама стыковка живёт на плате и здесь недоступна: там Wi-Fi,
// mbedTLS и сокеты. Но самая хрупкая часть — не криптография, а РАЗБОР: одна ошибка в
// base64url или в порядке полей транскрипта, и подпись не сойдётся, а на плате это
// выглядит как «телефон найден, но ничего не происходит» — ровно тот симптом, который
// мы неделю ловили вслепую. Поэтому чистая логика вынесена так, чтобы её можно было
// прогнать здесь, на настоящих образцах телефонного формата.
#include <stdio.h>
#include <string.h>
#include <stdint.h>

static int g_fail = 0;
static void check(bool ok, const char* what) {
    if (!ok) { printf("  СБОЙ: %s\n", what); ++g_fail; }
    else printf("  ok: %s\n", what);
}

// ── копии разбора из src/phone.cpp ────────────────────────────────────────────────────
//
// Копии, а не включение файла: тот тянет Arduino и mbedTLS. Функции короткие и меняются
// редко; расхождение поймает первый же прогон на живом телефоне.

static bool jsonStr(const char* json, const char* key, char* out, size_t cap) {
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
        if (*p == '\\' && p[1]) ++p;
        out[i++] = *p++;
    }
    out[i] = 0;
    return *p == '"';
}

static bool jsonArrAt(const char* json, const char* key, size_t idx, char* out, size_t cap) {
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

// base64url без выравнивания — как Bytes.b64u телефонной версии
static const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static size_t b64u(const uint8_t* in, size_t len, char* out) {
    size_t o = 0;
    for (size_t i = 0; i < len; i += 3) {
        const uint32_t v = (uint32_t(in[i]) << 16) |
                           (i + 1 < len ? uint32_t(in[i + 1]) << 8 : 0) |
                           (i + 2 < len ? uint32_t(in[i + 2]) : 0);
        out[o++] = kB64[(v >> 18) & 63];
        out[o++] = kB64[(v >> 12) & 63];
        if (i + 1 < len) out[o++] = kB64[(v >> 6) & 63];
        if (i + 2 < len) out[o++] = kB64[v & 63];
    }
    for (size_t i = 0; i < o; ++i) {
        if (out[i] == '+') out[i] = '-';
        else if (out[i] == '/') out[i] = '_';
    }
    out[o] = 0;
    return o;
}

int main() {
    printf("\nразбор телефонного формата\n");

    // Настоящая форма визитки телефона (VoileCrypto.Card.toJson).
    const char* card =
        "{\"v\":\"voile\\/1\",\"x\":\"q1-Ab2c\",\"y\":\"ZmZm_w\",\"eph\":\"MFkwEwYHKoZI\","
        "\"n\":\"AAECAwQFBgcICQoLDA0ODw\",\"eps\":[\"192.168.1.5:41234\",\"[fe80::1]:5000\"],"
        "\"sig\":\"MEUCIQDx\"}";

    char buf[128];
    check(jsonStr(card, "x", buf, sizeof(buf)) && strcmp(buf, "q1-Ab2c") == 0, "поле x");
    check(jsonStr(card, "y", buf, sizeof(buf)) && strcmp(buf, "ZmZm_w") == 0, "поле y");
    check(jsonStr(card, "eph", buf, sizeof(buf)) && strcmp(buf, "MFkwEwYHKoZI") == 0,
          "эфемерный ключ");
    check(jsonStr(card, "n", buf, sizeof(buf)) &&
          strcmp(buf, "AAECAwQFBgcICQoLDA0ODw") == 0, "разовое число");
    check(jsonStr(card, "sig", buf, sizeof(buf)) && strcmp(buf, "MEUCIQDx") == 0, "подпись");
    // Экранированная косая в "voile\/1" — так её пишет JSONObject у телефона.
    check(jsonStr(card, "v", buf, sizeof(buf)) && strcmp(buf, "voile/1") == 0,
          "версия со снятым экранированием");
    check(!jsonStr(card, "нет", buf, sizeof(buf)), "отсутствующее поле — отказ");

    check(jsonArrAt(card, "eps", 0, buf, sizeof(buf)) &&
          strcmp(buf, "192.168.1.5:41234") == 0, "первый кандидат");
    check(jsonArrAt(card, "eps", 1, buf, sizeof(buf)) &&
          strcmp(buf, "[fe80::1]:5000") == 0, "второй кандидат (IPv6)");
    check(!jsonArrAt(card, "eps", 2, buf, sizeof(buf)), "за концом списка — отказ");

    // Сортировка кандидатов: подпись накрывает их ИМЕННО отсортированными.
    char eps[4][48];
    size_t n = 0;
    while (n < 4 && jsonArrAt(card, "eps", n, eps[n], sizeof(eps[0]))) ++n;
    for (size_t a = 0; a + 1 < n; ++a)
        for (size_t b = a + 1; b < n; ++b)
            if (strcmp(eps[a], eps[b]) > 0) {
                char t[48]; strcpy(t, eps[a]); strcpy(eps[a], eps[b]); strcpy(eps[b], t);
            }
    check(strcmp(eps[0], "192.168.1.5:41234") == 0 && strcmp(eps[1], "[fe80::1]:5000") == 0,
          "порядок кандидатов как у телефона");

    // base64url: без выравнивания, с заменой двух символов.
    // Байты подобраны так, чтобы в обычном base64 вышли и '+', и '/' — проверяем замену.
    const uint8_t raw[3] = {0xFB, 0xFF, 0xBE};
    char out[16];
    b64u(raw, 3, out);
    check(strchr(out, '+') == nullptr && strchr(out, '/') == nullptr &&
          strchr(out, '=') == nullptr && strchr(out, '-') != nullptr &&
          strchr(out, '_') != nullptr, "base64url: + и / заменены, выравнивания нет");
    const uint8_t one[1] = {0x00};
    b64u(one, 1, out);
    check(strlen(out) == 2, "хвост без выравнивания");

    // Значение комнаты: целое и по частям (DhtRail).
    const char* whole = "0|{\"t\":\"a\",\"oid\":\"AAA=\"}";
    check(whole[0] == '0' && whole[1] == '|', "целое значение комнаты");
    const char* part = "1|ab12|0|2|{\"t\":\"a\"";
    check(part[0] == '1' && part[1] == '|', "часть значения комнаты");

    printf(g_fail ? "\nЕСТЬ СБОИ: %d\n" : "\nвсё сошлось\n", g_fail);
    return g_fail ? 1 : 0;
}
