#include "psram.h"

#include "store_sd.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <stdio.h>

namespace psram {

namespace {

bool g_have = false;

/**
 * Порог, с которого выделения уходят во внешнюю память.
 *
 * По умолчанию система отправляет наружу только куски от шестнадцати килобайт, а
 * защищённое соединение просит память кусками поменьше — и они целиком ложились на
 * внутреннюю. Две тысячи байт — та граница, ниже которой остаются мелочи, где важна
 * скорость, а всё существенное уходит в просторную внешнюю.
 */
constexpr size_t kExternalFrom = 2048;

}  // namespace

void begin() {
    g_have = heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0;
    if (!g_have) {
        // Карта в этот миг ещё не поднята, и запись в журнал пропадёт — поэтому в порт.
        ets_printf("[vual] внешней памяти нет — работаем на внутренней\n");
        store::log("память", "внешней памяти нет — работаем на внутренней");
        return;
    }

    heap_caps_malloc_extmem_enable(kExternalFrom);

    char msg[96];
    snprintf(msg, sizeof(msg), "внешняя память: %lu КБ свободно, внутренней %lu КБ",
             (unsigned long)(freeExternal() / 1024), (unsigned long)(freeInternal() / 1024));
    ets_printf("[vual] %s\n", msg);
    store::log("память", msg);
}

bool available() { return g_have; }

void* alloc(size_t bytes) {
    void* p = g_have ? heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM) : nullptr;
    // Не вышло — берём внутреннюю. Модуль без буфера не работает вовсе, а это хуже,
    // чем работать теснее.
    if (!p) p = heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
    return p;
}

uint32_t freeExternal() { return uint32_t(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)); }
uint32_t freeInternal() { return uint32_t(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)); }

}  // namespace psram
