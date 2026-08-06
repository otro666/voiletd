#include "voile_diversity.h"

#include <string.h>

namespace voile {

uint32_t copyDelayMs(uint8_t copyIndex, uint32_t jitterSeed) {
    if (copyIndex == 0) return 0;                  // первая копия уходит сразу
    // Растущие промежутки: 4, 11, 18… секунды. Растущие, а не равные, потому что если
    // первые две копии не прошли — обстановка устойчиво плохая, и ждать стоит дольше.
    const uint32_t base = 4000u + (copyIndex - 1) * 7000u;
    // Разброс до ±1,5 с, выведенный из семени: узлы не должны попадать в эфир
    // одновременно, иначе копии будут глушить друг друга.
    const uint32_t j = (jitterSeed * 2654435761u) % 3000u;
    return base + j - 1500u;
}

int32_t copyFreqOffsetKhz(uint8_t copyIndex) {
    // Смещения УБРАНЫ: все копии идут на основной частоте.
    //
    // Идея частотного разнесения верна сама по себе, но требует, чтобы приёмник прыгал
    // по частотам вместе с передатчиком. Приёмник же всегда слушает только основную
    // частоту — договориться о расписании прыжков без общих часов нечем. Смещённые
    // копии никто не мог принять: при полосе в десятки килогерц смещение в сотни
    // килогерц — глухая зона. Две копии из трёх уходили в пустоту, занимая эфир и не
    // давая плате слушать.
    //
    // Разнесение оставлено только во времени (copyDelayMs) — оно работает без всякой
    // синхронизации. Функция сохранена, чтобы не менять вызывающий код и тесты:
    // если появится настоящая схема прыжков с синхронизацией, вернуть смещения можно
    // будет в одном месте.
    (void)copyIndex;
    return 0;
}

void fecParity(const uint8_t* const* blocks, uint8_t count, size_t blockLen, uint8_t* out) {
    memset(out, 0, blockLen);
    for (uint8_t i = 0; i < count; ++i) {
        if (!blocks[i]) continue;
        for (size_t j = 0; j < blockLen; ++j) out[j] ^= blocks[i][j];
    }
}

bool fecRecover(const uint8_t* const* present, uint8_t count, size_t blockLen,
                const uint8_t* parity, uint8_t missingIndex, uint8_t* out) {
    if (missingIndex >= count) return false;
    // Ровно один потерянный: XOR-код больше не вытянет.
    uint8_t lost = 0;
    for (uint8_t i = 0; i < count; ++i) if (!present[i]) ++lost;
    if (lost != 1 || present[missingIndex]) return false;

    memcpy(out, parity, blockLen);
    for (uint8_t i = 0; i < count; ++i) {
        if (i == missingIndex || !present[i]) continue;
        for (size_t j = 0; j < blockLen; ++j) out[j] ^= present[i][j];
    }
    return true;
}

bool SeenCache::seen(const uint8_t src[4], uint16_t seq) {
    const uint32_t s = (uint32_t(src[0]) << 24) | (uint32_t(src[1]) << 16) |
                       (uint32_t(src[2]) << 8)  |  uint32_t(src[3]);
    for (size_t i = 0; i < kSize; ++i) {
        if (ring_[i].used && ring_[i].src == s && ring_[i].seq == seq) return true;
    }
    ring_[next_] = Entry{s, seq, true};
    next_ = (next_ + 1) % kSize;
    return false;
}

}  // namespace voile
