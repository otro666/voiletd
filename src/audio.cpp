#include "audio.h"

#include <string.h>

namespace audio {

// Таблицы IMA ADPCM — стандартные, менять нельзя: по ним же разворачивает любой
// сторонний проигрыватель, если понадобится совместимость наружу.
static const int16_t kStepTable[89] = {
    7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,73,80,88,97,
    107,118,130,143,157,173,190,209,230,253,279,307,337,371,408,449,494,544,598,658,
    724,796,876,963,1060,1166,1282,1411,1552,1707,1878,2066,2272,2499,2749,3024,3327,
    3660,4026,4428,4871,5358,5894,6484,7132,7845,8630,9493,10442,11487,12635,13899,
    15289,16818,18500,20350,22385,24623,27086,29794,32767
};
static const int8_t kIndexTable[16] = {
    -1,-1,-1,-1,2,4,6,8, -1,-1,-1,-1,2,4,6,8
};

static inline int16_t clamp16(int v) {
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return int16_t(v);
}

static uint8_t encodeSample(AdpcmState& st, int16_t sample) {
    const int step = kStepTable[st.index];
    int diff = sample - st.predictor;
    uint8_t code = 0;
    if (diff < 0) { code = 8; diff = -diff; }

    int delta = step >> 3;
    if (diff >= step)      { code |= 4; diff -= step;      delta += step; }
    if (diff >= (step>>1)) { code |= 2; diff -= step >> 1; delta += step >> 1; }
    if (diff >= (step>>2)) { code |= 1;                    delta += step >> 2; }

    st.predictor = clamp16(st.predictor + ((code & 8) ? -delta : delta));
    st.index += kIndexTable[code];
    if (st.index < 0) st.index = 0;
    if (st.index > 88) st.index = 88;
    return code;
}

static int16_t decodeSample(AdpcmState& st, uint8_t code) {
    const int step = kStepTable[st.index];
    int delta = step >> 3;
    if (code & 4) delta += step;
    if (code & 2) delta += step >> 1;
    if (code & 1) delta += step >> 2;

    st.predictor = clamp16(st.predictor + ((code & 8) ? -delta : delta));
    st.index += kIndexTable[code];
    if (st.index < 0) st.index = 0;
    if (st.index > 88) st.index = 88;
    return st.predictor;
}

size_t adpcmEncode(AdpcmState& st, const int16_t* samples, size_t count, uint8_t* out) {
    size_t n = 0;
    for (size_t i = 0; i + 1 < count; i += 2) {
        const uint8_t lo = encodeSample(st, samples[i]);
        const uint8_t hi = encodeSample(st, samples[i + 1]);
        out[n++] = uint8_t(lo | (hi << 4));
    }
    return n;
}

size_t adpcmDecode(AdpcmState& st, const uint8_t* in, size_t bytes, int16_t* out) {
    size_t n = 0;
    for (size_t i = 0; i < bytes; ++i) {
        out[n++] = decodeSample(st, uint8_t(in[i] & 0x0F));
        out[n++] = decodeSample(st, uint8_t(in[i] >> 4));
    }
    return n;
}

size_t writeWavHeader(uint8_t* out, uint32_t pcmBytes, int sampleRate) {
    // Минимальный WAV: телефонная версия и любой проигрыватель понимают его без оговорок.
    auto put32 = [](uint8_t* p, uint32_t v) {
        p[0]=uint8_t(v); p[1]=uint8_t(v>>8); p[2]=uint8_t(v>>16); p[3]=uint8_t(v>>24);
    };
    auto put16 = [](uint8_t* p, uint16_t v) { p[0]=uint8_t(v); p[1]=uint8_t(v>>8); };

    memcpy(out, "RIFF", 4);
    put32(out + 4, 36 + pcmBytes);
    memcpy(out + 8, "WAVEfmt ", 8);
    put32(out + 16, 16);                       // размер блока fmt
    put16(out + 20, 1);                        // PCM без сжатия
    put16(out + 22, 1);                        // моно
    put32(out + 24, uint32_t(sampleRate));
    put32(out + 28, uint32_t(sampleRate) * 2); // байт в секунду
    put16(out + 32, 2);                        // выравнивание блока
    put16(out + 34, 16);                       // бит на отсчёт
    memcpy(out + 36, "data", 4);
    put32(out + 40, pcmBytes);
    return 44;
}

}  // namespace audio
