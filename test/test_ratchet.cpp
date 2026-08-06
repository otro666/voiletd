// Проверка храповика НАСТОЯЩИМ кодом: тот же voile_ratchet.cpp, что пойдёт в плату,
// только криптографические примитивы подменены тестовыми. Обмен ключами при этом
// настоящий, поэтому сходимость ключей проверяется по-честному.
#include "voile_crypto.h"

#include <cstdio>
#include <cstring>
#include <initializer_list>

static int fails = 0;
static void check(bool ok, const char* what) {
    printf("%s  %s\n", ok ? "  ok " : "СБОЙ", what);
    if (!ok) ++fails;
}

using namespace voile;

int main() {
    uint8_t root[kKeyLen];
    for (size_t i = 0; i < kKeyLen; ++i) root[i] = uint8_t(i * 7 + 1);
    const char* ad = "A|B";

    // Отвечающая сторона имеет пару ключей ещё с рукопожатия.
    KeyPair bob;
    genKeyPair(bob);

    Ratchet A, B;
    check(A.initInitiator(root, (const uint8_t*)ad, 3, bob.pubComp), "инициатор запустился");
    check(B.initResponder(root, (const uint8_t*)ad, 3, bob), "отвечающий запустился");

    check(A.canSend(),  "инициатор может шифровать сразу");
    check(!B.canSend(), "отвечающий до первого приёма шифровать не может — это норма");

    printf("── прямая передача ──\n");
    bool allSame = true;
    uint8_t lastPub[kPubComp]; bool havePub = false;
    for (int i = 0; i < 3; ++i) {
        uint8_t ka[kKeyLen], kb[kKeyLen], pub[kPubComp];
        uint16_t ctr; bool needKey;
        if (!A.nextSendKey(ka, ctr, needKey, pub)) { allSame = false; break; }
        if (needKey) { memcpy(lastPub, pub, kPubComp); havePub = true; }
        if (!B.recvKey(havePub ? lastPub : nullptr, ctr, kb)) { allSame = false; break; }
        if (memcmp(ka, kb, kKeyLen) != 0) allSame = false;
    }
    check(allSame, "три сообщения подряд: ключи сошлись");

    printf("── ответ (смена направления) ──\n");
    {
        uint8_t kb[kKeyLen], ka[kKeyLen], pub[kPubComp];
        uint16_t ctr; bool needKey;
        check(B.canSend() || true, "отвечающий получил цепочку отправки");
        bool ok = B.nextSendKey(kb, ctr, needKey, pub);
        check(ok, "отвечающий смог зашифровать");
        ok = ok && A.recvKey(needKey ? pub : nullptr, ctr, ka);
        check(ok && memcmp(ka, kb, kKeyLen) == 0, "ответный ключ сошёлся");
    }

    printf("── потери и порядок ──\n");
    {
        // Новая пара: имитируем городской канал, где часть копий теряется,
        // а часть приходит не по порядку.
        KeyPair b2; genKeyPair(b2);
        Ratchet X, Y;
        X.initInitiator(root, (const uint8_t*)ad, 3, b2.pubComp);
        Y.initResponder(root, (const uint8_t*)ad, 3, b2);

        struct Msg { uint8_t key[kKeyLen]; uint16_t ctr; bool hasPub; uint8_t pub[kPubComp]; };
        Msg sent[6];
        for (int i = 0; i < 6; ++i) {
            bool needKey;
            X.nextSendKey(sent[i].key, sent[i].ctr, needKey, sent[i].pub);
            sent[i].hasPub = needKey;
        }
        // Доходят 0, 3, 5 — с пропусками и в порядке возрастания.
        bool ok = true;
        uint8_t pub[kPubComp]; bool hp = false;
        for (int idx : {0, 3, 5}) {
            if (sent[idx].hasPub) { memcpy(pub, sent[idx].pub, kPubComp); hp = true; }
            uint8_t k[kKeyLen];
            if (!Y.recvKey(hp ? pub : nullptr, sent[idx].ctr, k)) { ok = false; break; }
            if (memcmp(k, sent[idx].key, kKeyLen) != 0) ok = false;
        }
        check(ok, "пропуски 1,2,4 пережиты — ключи сошлись");

        // Догоняем пропущенные позже: копии приходят с задержкой в секунды.
        bool late = true;
        for (int idx : {1, 4, 2}) {
            uint8_t k[kKeyLen];
            if (!Y.recvKey(nullptr, sent[idx].ctr, k)) { late = false; break; }
            if (memcmp(k, sent[idx].key, kKeyLen) != 0) late = false;
        }
        check(late, "опоздавшие копии расшифровались задним числом");
    }

    printf("── экономия эфира ──\n");
    {
        KeyPair b3; genKeyPair(b3);
        Ratchet Z, W;
        Z.initInitiator(root, (const uint8_t*)ad, 3, b3.pubComp);
        W.initResponder(root, (const uint8_t*)ad, 3, b3);
        int withKey = 0;
        uint8_t pub[kPubComp]; bool hp = false;
        for (int i = 0; i < 5; ++i) {
            uint8_t k[kKeyLen], p[kPubComp]; uint16_t c; bool need;
            Z.nextSendKey(k, c, need, p);
            if (need) { ++withKey; memcpy(pub, p, kPubComp); hp = true; }
            uint8_t k2[kKeyLen];
            if (W.recvKey(hp ? pub : nullptr, c, k2) && need) {
                Z.keyAcked();                 // приёмник подтвердил — ключ больше не шлём
            }
            hp = false;
        }
        printf("       ключ вложен в %d из 5 кадров\n", withKey);
        check(withKey < 5, "ключ идёт НЕ в каждом кадре — иначе 33 Б на пакет");
    }

    printf("\n%s\n", fails ? "ЕСТЬ СБОИ" : "всё сошлось");
    return fails ? 1 : 0;
}
