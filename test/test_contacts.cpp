// Проверка контактов и знакомства. Ошибка в выводе адреса означала бы, что стороны
// не находят друг друга вовсе, — а понять это по молчащему эфиру почти невозможно.
#include "contacts.h"

#include <cstdio>
#include <cstring>

static int fails = 0;
static void check(bool ok, const char* what) {
    printf("%s  %s\n", ok ? "  ok " : "СБОЙ", what);
    if (!ok) ++fails;
}

using namespace contacts;

int main() {
    begin();

    voile::KeyPair a, b;
    voile::genKeyPair(a);
    voile::genKeyPair(b);

    printf("── адрес из ключа ──\n");
    uint8_t aa[4], ab[4], aa2[4];
    addrFromPub(a.pubComp, aa);
    addrFromPub(b.pubComp, ab);
    addrFromPub(a.pubComp, aa2);
    check(memcmp(aa, aa2, 4) == 0, "адрес выводится одинаково при каждом вызове");
    check(memcmp(aa, ab, 4) != 0, "разные ключи дают разные адреса");

    printf("── хранилище ──\n");
    Contact* c1 = upsert("Карина", a.pubComp);
    check(c1 != nullptr, "контакт добавился");
    check(strcmp(c1->name, "Карина") == 0, "имя сохранилось");
    check(memcmp(c1->addr, aa, 4) == 0, "адрес совпал с выведенным из ключа");
    check(count() == 1, "счётчик контактов верен");

    Contact* again = upsert("Кари", a.pubComp);
    check(again == c1, "повторное добавление того же ключа не создаёт дубль");
    check(strcmp(c1->name, "Кари") == 0, "имя обновилось");
    check(count() == 1, "дубля не появилось");

    upsert("Артём", b.pubComp);
    check(count() == 2, "второй контакт добавился");
    check(byAddr(ab) != nullptr, "поиск по адресу работает");
    uint8_t nobody[4] = {0xDE, 0xAD, 0xBE, 0xEF};
    check(byAddr(nobody) == nullptr, "неизвестный адрес не находится");

    printf("── пути связи ──\n");
    markSeen(aa, true, 5000);
    check(c1->viaWifi && !c1->viaLora, "отметился путь Wi-Fi");
    markSeen(aa, false, 6000);
    check(c1->viaWifi && c1->viaLora, "радио добавилось, Wi-Fi не потерялся");
    check(c1->lastSeenMs == 6000, "время последнего появления обновилось");

    printf("── знакомство по фразе ──\n");
    Rendezvous r1, r2, r3;
    deriveRendezvous("зелёный чайник", r1);
    deriveRendezvous("зелёный чайник", r2);
    deriveRendezvous("зелёный чайнек", r3);
    check(memcmp(r1.meetAddr, r2.meetAddr, 4) == 0,
          "одна фраза у обеих сторон даёт один адрес встречи");
    check(memcmp(r1.wrapKey, r2.wrapKey, 32) == 0, "и один ключ");
    check(memcmp(r1.meetAddr, r3.meetAddr, 4) != 0,
          "опечатка в фразе — другой адрес, стороны не встретятся");
    // Адрес встречи виден в эфире. Если бы ключ выводился из него же, подслушивающий
    // получил бы и ключ — поэтому метки вывода разные.
    check(memcmp(r1.meetAddr, r1.wrapKey, 4) != 0,
          "ключ не выводится из видимого в эфире адреса");

    printf("\n%s\n", fails ? "ЕСТЬ СБОИ" : "всё сошлось");
    return fails ? 1 : 0;
}
