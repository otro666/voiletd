// ВСЕ адреса из телефонной версии — полными списками, в том же порядке.
//
// Порядок значим: он подобран по доступности. Первыми идут те, что отвечают из России,
// дальше остальные. Плата обходит их сверху вниз и останавливается на первом ответившем.
//
// Держать столько нужно потому, что доступность у каждого своя: один лежит, второй
// режется у оператора, третий недоступен из конкретной страны. Собеседник придёт к
// какому-то одному, и угадать, к какому именно, нечем.
#pragma once

#include <stdint.h>
#include <stddef.h>

namespace relays {

struct HostPort { const char* host; uint16_t port; };

/** Серверы определения внешнего адреса — все семь. */
extern const HostPort kStun[];
extern const size_t kStunCount;

/** Брокеры рельсы знакомства — все шесть. */
extern const HostPort kBrokers[];
extern const size_t kBrokerCount;
/** Путь к брокеру внутри веб-сокета — одинаковый у всех. */
extern const char* const kBrokerPath;

/** Узлы Nostr — все двадцать пять: десять основных и пятнадцать запасных. */
extern const char* const kNostr[];
extern const size_t kNostrCount;

/**
 * Трекеры — все восемнадцать.
 *
 * Самая живучая рельса: трекеры существуют для раздачи файлов, нужны слишком многим и
 * закрывают их редко. Там, где перекрыты и брокеры, и узлы Nostr, эти обычно отвечают.
 *
 * Адрес разобран на части заранее: разбирать его на плате в работе — лишний код и лишние
 * ошибки, а список неизменный.
 */
struct Tracker { const char* host; uint16_t port; const char* path; };
extern const Tracker kTrackers[];
extern const size_t kTrackerCount;

/** Ретрансляторы уведомлений — оба. */
extern const char* const kPush[];
extern const size_t kPushCount;

}  // namespace relays
