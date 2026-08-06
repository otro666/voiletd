#include "transport.h"

namespace transport {

Rail pickRail(const PeerRoute& p, uint32_t nowMs, bool needFiles) {
    const bool wifiFresh = p.wifiSeen && (nowMs - p.wifiLastMs) < kWifiStaleMs;
    const bool loraFresh = p.loraSeen && (nowMs - p.loraLastMs) < kLoraStaleMs;

    // Файлы, фотографии, стикеры и рация идут только по Wi-Fi — по радио они физически
    // не помещаются в разумное время. Лучше честно отказать, чем занять эфир на час.
    if (needFiles) return wifiFresh ? RAIL_WIFI : RAIL_NONE;

    if (wifiFresh) return RAIL_WIFI;
    if (loraFresh) return RAIL_LORA;
    // Обоих путей нет — сообщение ляжет в очередь на microSD и уйдёт, когда собеседник
    // объявится хоть где-то.
    return RAIL_NONE;
}

size_t chunkSize(Rail r) {
    // По Wi-Fi размер тот же, что в телефонной версии, — иначе разбор на той стороне
    // не сойдётся.
    if (r == RAIL_WIFI) return 45 * 1024;
    // По радио — то, что остаётся от пакета после заголовка и тега.
    return 200;
}

Estimate estimate(Rail r, size_t bytes) {
    Estimate e{};
    const size_t cs = chunkSize(r);
    e.parts = uint32_t((bytes + cs - 1) / cs);
    const Caps c = capsOf(r);
    e.seconds = uint32_t((uint64_t(bytes) * 8) / (c.bps ? c.bps : 1));

    if (r == RAIL_LORA) {
        // На радио к времени передачи добавляется ожидание: лимит эфирного времени в 10%
        // означает, что девять десятых времени мы молчим.
        e.seconds *= 10;
        // Разумным считаем то, что уложится в пару минут. Дольше — человек не дождётся,
        // а эфир будет занят в ущерб всем соседям.
        e.sane = e.seconds <= 120;
    } else {
        e.sane = true;
    }
    return e;
}

}  // namespace transport
