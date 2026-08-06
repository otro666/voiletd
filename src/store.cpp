#include "store.h"

#include <string.h>

namespace store {

size_t writeRecHeader(uint8_t* out, const RecHeader& h) {
    out[0] = uint8_t(h.len);       out[1] = uint8_t(h.len >> 8);
    out[2] = uint8_t(h.crc);       out[3] = uint8_t(h.crc >> 8);
    out[4] = h.kind;
    out[5] = h.flags;
    out[6] = uint8_t(h.ts);        out[7] = uint8_t(h.ts >> 8);
    out[8] = uint8_t(h.ts >> 16);  out[9] = uint8_t(h.ts >> 24);
    out[10] = uint8_t(h.seq);      out[11] = uint8_t(h.seq >> 8);
    return kRecHdrLen;
}

bool readRecHeader(const uint8_t* in, size_t len, RecHeader& h) {
    if (len < kRecHdrLen) return false;
    h.len   = uint16_t(in[0] | (in[1] << 8));
    h.crc   = uint16_t(in[2] | (in[3] << 8));
    h.kind  = in[4];
    h.flags = in[5];
    h.ts    = uint32_t(in[6]) | (uint32_t(in[7]) << 8) |
              (uint32_t(in[8]) << 16) | (uint32_t(in[9]) << 24);
    h.seq   = uint16_t(in[10] | (in[11] << 8));
    return true;
}

uint16_t crc16(const uint8_t* data, size_t len) {
    // CRC-16/CCITT. Табличная версия заняла бы 512 байт памяти ради экономии, которая
    // здесь не нужна: записи короткие, а карта всё равно медленнее вычислений.
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; ++i) {
        crc ^= uint16_t(data[i]) << 8;
        for (int b = 0; b < 8; ++b)
            crc = (crc & 0x8000) ? uint16_t((crc << 1) ^ 0x1021) : uint16_t(crc << 1);
    }
    return crc;
}

bool recordValid(const uint8_t* rec, size_t available, size_t& consumed) {
    RecHeader h;
    if (!readRecHeader(rec, available, h)) return false;
    // Хвост журнала после обрыва питания: заголовок записался, содержимое — нет.
    if (available < kRecHdrLen + h.len) return false;
    if (crc16(rec + kRecHdrLen, h.len) != h.crc) return false;
    consumed = kRecHdrLen + h.len;
    return true;
}

}  // namespace store
