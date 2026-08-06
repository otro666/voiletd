// Передача файлов: голосовых, снимков, документов.
//
// Один путь на всё: голосовое, снимок и документ различаются только пометкой вида, а
// дробление, сборка и досылка у них общие. Разводить их значило бы чинить потом каждый
// отдельно.
//
// ПРО ДВА ПУТИ И ПОЧЕМУ ОНИ РАЗНЫЕ.
//
// По сети файл уходит крупными кусками и быстро. По радио в кадр помещается 226 байт, и
// минута голосового — это больше тысячи кадров, около трёх минут эфира. Поэтому по радио
// файлы отправляются только с явного согласия, а размер и время говорятся заранее: молча
// занять эфир на три минуты нельзя.
#pragma once

#include <stdint.h>
#include <stddef.h>

namespace xfer {

/** Что передаём. Влияет только на то, как показать у получателя. */
enum Kind : uint8_t {
    K_FILE  = 0,
    K_VOICE = 1,
    K_PHOTO = 2,
};

/** Путь передачи. */
enum Route : uint8_t {
    R_NET   = 0,   // по сети: быстро, куски крупные
    R_RADIO = 1,   // по радио: медленно, кадры мелкие
};

/** Сколько передач идёт одновременно. Больше на плате не унести: у каждой свой буфер. */
constexpr size_t kMaxActive = 2;

struct Progress {
    bool     active;
    Kind     kind;
    Route    route;
    bool     outgoing;
    uint32_t total;
    uint32_t done;
    char     name[40];
};

/** Начать отправку файла с карты. */
bool send(const char* peer, const char* path, Kind kind, Route route);

/** Сколько времени займёт передача по радио. Показываем ДО начала: три минуты эфира —
 *  это то, о чём человек должен знать заранее. */
uint32_t radioEstimateMs(uint32_t bytes);

/** Подать передачи: отправка кусков, досылка. Из главного цикла. */
void pump();

/** Принять кусок из сети или эфира. */
void onChunk(const char* peer, const uint8_t* data, size_t len, Route route);

/** Ход текущих передач — для показа в ленте. */
size_t activeCount();
const Progress* activeAt(size_t i);

/** Что делать с готовым файлом. Ставится ядром. */
using OnComplete = void (*)(const char* peer, const char* path, Kind kind, bool incoming);

/** Отправитель куска по радио. Возврат false — эфирная очередь занята: перекачка
 *  предложит ТОТ ЖЕ кусок позже. Так скорость перекачки подстраивается под эфир, а не
 *  наоборот. Регистрируется из main — там живут очередь и кадры. */
using RadioSender = bool (*)(const char* peer, const uint8_t* data, size_t len);
void setRadioSender(RadioSender fn);
void setOnComplete(OnComplete cb);

}  // namespace xfer
