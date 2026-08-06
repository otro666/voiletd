#include "voile_frame.h"

#include <string.h>

namespace voile {

size_t writeHeader(uint8_t* out, const Header& h) {
    out[0] = h.type;
    memcpy(out + 1, h.dst, kAddrLen);
    memcpy(out + 1 + kAddrLen, h.src, kAddrLen);
    // Порядок байт сетевой (старший первым) — чтобы разбор не зависел от процессора.
    out[9]  = uint8_t(h.seq >> 8);
    out[10] = uint8_t(h.seq & 0xFF);
    out[11] = h.part;
    out[12] = h.copy;
    return kHdrLen;
}

bool readHeader(const uint8_t* in, size_t len, Header& h) {
    if (len < kHdrLen) return false;
    h.type = in[0];
    memcpy(h.dst, in + 1, kAddrLen);
    memcpy(h.src, in + 1 + kAddrLen, kAddrLen);
    h.seq  = uint16_t((uint16_t(in[9]) << 8) | in[10]);
    h.part = in[11];
    h.copy = in[12];
    return true;
}

}  // namespace voile
