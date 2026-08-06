// Третья рельса знакомства — трекеры.
//
// Их восемнадцать, и это самая живучая из трёх. Трекеры существуют для раздачи файлов и
// закрывают их редко: они нужны слишком многим и слишком безобидны на вид. Там, где
// перекрыты и брокеры, и узлы Nostr, трекеры обычно ещё отвечают.
//
// Устройство иное, чем у первых двух. Здесь нет ни тем, ни событий: есть «раздача» с
// именем, выведенным из кодового слова, и мы просто объявляемся её участником. Трекер
// сводит нас с тем, кто объявился под тем же именем, — ровно то, что нужно для знакомства.
//
// ОСОБЕННОСТЬ ФОРМАТА, о которую легко споткнуться: двоичные значения — имя раздачи,
// наш признак, признак предложения — передаются НЕ в шестнадцатеричном виде и не в
// основании 64, а посимвольно, где каждый байт становится символом с тем же кодом.
// Так исторически сложилось в этом протоколе, и отступление означает, что трекер нас
// просто не поймёт.
#pragma once

#include <stdint.h>
#include <stddef.h>

namespace tracker {

/** Сколько трекеров держим одновременно. */
constexpr size_t kMaxOpen = 3;

struct Incoming {
    char    kind;          // 'p' присутствие, 'o' предложение, 'a' ответ
    char    room[41];
    uint8_t peerId[20];
    uint8_t offerId[20];
    bool    hasOffer;
    char    body[512];
};

using OnMessage = void (*)(const Incoming&);

bool begin(const uint8_t myPeerId[20]);
bool connected();
size_t openCount();

void joinRoom(const char* roomHex);
void leaveRoom(const char* roomHex);

/** Объявиться участником раздачи. Трекер в ответ пришлёт тех, кто уже там. */
void announce(const char* roomHex);

void sendOffer(const char* roomHex, const uint8_t offerId[20], const char* body);
void sendAnswer(const char* roomHex, const uint8_t offerId[20], const char* body);

void setOnMessage(OnMessage cb);

}  // namespace tracker
