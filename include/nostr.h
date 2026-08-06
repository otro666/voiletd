// Вторая рельса знакомства — Nostr.
//
// Нужна как запасной путь. Брокеры первой рельсы и узлы Nostr закрывают разными
// способами и в разных местах: когда не работает одно, обычно работает другое. Держать обе
// значит заметно повысить шанс встретиться в недружелюбной сети.
//
// Устройство отличается от первой рельсы: здесь нет «комнат» в готовом виде, есть события
// с метками. Мы кладём событие с меткой, выведенной из кодового слова, и подписываемся на
// ту же метку — встреча происходит так же, но механикой иного рода.
//
// Каждое событие ПОДПИСЫВАЕТСЯ: узел без подписи его не примет. Подпись по стандарту
// BIP340, см. schnorr.h.
#pragma once

#include <stdint.h>
#include <stddef.h>

namespace nostr {

/** Сколько узлов держим одновременно. Их два с половиной десятка, но открывать все разом
 *  на плате нечем: каждое защищённое соединение съедает изрядно памяти. */
constexpr size_t kMaxOpen = 3;

/** Вид события. Числа из телефонной версии — по ним стороны и находят друг друга. */
constexpr int kKindSignal = 20387;   // обмен адресами
constexpr int kKindInbox  = 4387;    // хранимый ящик комнаты

struct Incoming {
    char kind;            // 'p' присутствие, 'o' предложение, 'a' ответ
    char room[41];
    uint8_t peerId[20];
    uint8_t offerId[20];
    bool hasOffer;
    char body[512];
};

using OnMessage = void (*)(const Incoming&);

/** Поднять рельсу: создать ключ подписи и запустить свой поток. */
bool begin(const uint8_t myPeerId[20]);

/** На связи ли хоть один узел. */
bool connected();
size_t openCount();

/** Прийти в комнату и слушать её. */
void joinRoom(const char* roomHex);
void leaveRoom(const char* roomHex);

/** Объявить о себе. */
void announce(const char* roomHex);

/** Предложение связи и ответ на чужое. */
void sendOffer(const char* roomHex, const uint8_t offerId[20], const char* body);
void sendAnswer(const char* roomHex, const uint8_t offerId[20], const char* body);

void setOnMessage(OnMessage cb);

}  // namespace nostr
