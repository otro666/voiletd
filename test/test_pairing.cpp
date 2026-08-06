// Проверка совместимости знакомства с ТЕЛЕФОННОЙ версией.
//
// Это самый хрупкий стык всего проекта: если вывод комнаты из фразы разойдётся хоть на
// байт, стороны просто не найдут друг друга — и понять это по молчащему эфиру почти
// невозможно. Эталонные значения посчитаны по формуле из Rooms.kt телефонной версии.
#include "contacts.h"

#include <cstdio>
#include <cstring>

static int fails = 0;
static void check(bool ok, const char* what) {
    printf("%s  %s\n", ok ? "  ok " : "СБОЙ", what);
    if (!ok) ++fails;
}

static void hex(const uint8_t* p, size_t n, char* out) {
    for (size_t i = 0; i < n; ++i) sprintf(out + i * 2, "%02x", p[i]);
    out[n * 2] = 0;
}

using namespace contacts;

struct Vec { const char* phrase; const char* norm; const char* room; };

// Посчитано по формуле телефонной версии: SHA-1("vual1-p:" + normSecret(фраза))
static const Vec kVectors[] = {
    {"зелёный чайник",        "зелёный чайник", "280fb8b4b125ad342e62bc8599c121ce64169937"},
    {"  Зелёный   ЧАЙНИК  ",  "зелёный чайник", "280fb8b4b125ad342e62bc8599c121ce64169937"},
    {"Hello World",           "hello world",    "505627ab44370fd6aa84078ae681461986394b69"},
    {"ЯБЛОКО",                "яблоко",         "b77a6be7e6ec9e122d59160ab3277b5ed30edaeb"},
};

int main() {
    printf("── нормализация фразы ──\n");
    for (const Vec& v : kVectors) {
        char n[128];
        normalizePhrase(v.phrase, n, sizeof(n));
        char msg[160];
        snprintf(msg, sizeof(msg), "\"%s\" -> \"%s\"", v.phrase, n);
        check(strcmp(n, v.norm) == 0, msg);
    }

    printf("── комната для телефона ──\n");
    for (const Vec& v : kVectors) {
        Rendezvous r{};
        deriveRendezvous(v.phrase, r);
        char got[64];
        hex(r.phoneRoom, 20, got);
        char msg[200];
        snprintf(msg, sizeof(msg), "\"%s\" даёт ту же комнату, что телефон", v.phrase);
        if (strcmp(got, v.room) != 0)
            printf("       ожидалось %s\n       получено  %s\n", v.room, got);
        check(strcmp(got, v.room) == 0, msg);
    }

    printf("── две среды из одной фразы ──\n");
    {
        Rendezvous a{}, b{};
        deriveRendezvous("зелёный чайник", a);
        deriveRendezvous("  Зелёный   ЧАЙНИК  ", b);
        check(memcmp(a.phoneRoom, b.phoneRoom, 20) == 0,
              "регистр и пробелы не мешают встретиться с телефоном");
        check(memcmp(a.meetAddr, b.meetAddr, 4) == 0,
              "и по радио тоже");
        check(memcmp(a.wrapKey, b.wrapKey, 32) == 0, "ключ обмена совпал");

        // Адрес для радио и комната для телефона обязаны РАЗЛИЧАТЬСЯ: это разные среды
        // с разными требованиями к длине, и совпадение означало бы ошибку вывода.
        check(memcmp(a.meetAddr, a.phoneRoom, 4) != 0,
              "радиоадрес не равен началу телефонной комнаты");
        // Ключ не должен выводиться из того, что видно в эфире.
        check(memcmp(a.wrapKey, a.meetAddr, 4) != 0, "ключ не выводится из адреса");

        Rendezvous c{};
        deriveRendezvous("зелёный чайнек", c);
        check(memcmp(a.phoneRoom, c.phoneRoom, 20) != 0, "опечатка меняет комнату");
        check(memcmp(a.meetAddr, c.meetAddr, 4) != 0, "и радиоадрес");
    }

    printf("\n%s\n", fails ? "ЕСТЬ СБОИ" : "всё сошлось");
    return fails ? 1 : 0;
}
