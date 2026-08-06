// Тестовая реализация криптографических примитивов — ТОЛЬКО для проверки логики
// храповика на обычном компьютере. На плате вместо неё работает mbedTLS.
//
// Обмен ключами настоящий (модульное возведение в степень), а не подделка: иначе
// проверка сходимости ключей ничего бы не значила. Хеш — тоже настоящий SHA-256.
#include "voile_crypto.h"

#include <cstring>
#include <cstdint>
#include <vector>
#include <random>

// ── SHA-256 (компактная реализация для тестов) ─────────────────────────────────────────
namespace {

struct Sha256 {
    uint32_t h[8]; uint64_t len; uint8_t buf[64]; size_t n;
    static uint32_t ror(uint32_t x, int c) { return (x >> c) | (x << (32 - c)); }
    void init() {
        static const uint32_t I[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                                      0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
        memcpy(h, I, sizeof(I)); len = 0; n = 0;
    }
    void block(const uint8_t* p) {
        static const uint32_t K[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
        uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = (uint32_t(p[i*4])<<24)|(uint32_t(p[i*4+1])<<16)|
                   (uint32_t(p[i*4+2])<<8)|uint32_t(p[i*4+3]);
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = ror(w[i-15],7)^ror(w[i-15],18)^(w[i-15]>>3);
            uint32_t s1 = ror(w[i-2],17)^ror(w[i-2],19)^(w[i-2]>>10);
            w[i] = w[i-16]+s0+w[i-7]+s1;
        }
        uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
        for (int i = 0; i < 64; ++i) {
            uint32_t S1 = ror(e,6)^ror(e,11)^ror(e,25);
            uint32_t ch = (e&f)^((~e)&g);
            uint32_t t1 = hh+S1+ch+K[i]+w[i];
            uint32_t S0 = ror(a,2)^ror(a,13)^ror(a,22);
            uint32_t mj = (a&b)^(a&c)^(b&c);
            uint32_t t2 = S0+mj;
            hh=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
        }
        h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
    }
    void update(const uint8_t* p, size_t l) {
        len += l;
        while (l) {
            size_t k = 64 - n; if (k > l) k = l;
            memcpy(buf + n, p, k); n += k; p += k; l -= k;
            if (n == 64) { block(buf); n = 0; }
        }
    }
    void final(uint8_t out[32]) {
        uint64_t bits = len * 8;
        uint8_t pad = 0x80; update(&pad, 1);
        uint8_t z = 0; while (n != 56) update(&z, 1);
        uint8_t l8[8];
        for (int i = 0; i < 8; ++i) l8[i] = uint8_t(bits >> (56 - i*8));
        update(l8, 8);
        for (int i = 0; i < 8; ++i) {
            out[i*4]   = uint8_t(h[i] >> 24); out[i*4+1] = uint8_t(h[i] >> 16);
            out[i*4+2] = uint8_t(h[i] >> 8);  out[i*4+3] = uint8_t(h[i]);
        }
    }
};

void sha256(const uint8_t* p, size_t l, uint8_t out[32]) {
    Sha256 s; s.init(); s.update(p, l); s.final(out);
}

void hmacSha256(const uint8_t* key, size_t kl, const uint8_t* msg, size_t ml,
                uint8_t out[32]) {
    uint8_t k[64] = {};
    if (kl > 64) sha256(key, kl, k); else memcpy(k, key, kl);
    uint8_t ip[64], op[64];
    for (int i = 0; i < 64; ++i) { ip[i] = k[i] ^ 0x36; op[i] = k[i] ^ 0x5c; }
    uint8_t inner[32];
    Sha256 s; s.init(); s.update(ip, 64); s.update(msg, ml); s.final(inner);
    Sha256 t; t.init(); t.update(op, 64); t.update(inner, 32); t.final(out);
}

// ── обмен ключами: настоящее модульное возведение в степень ────────────────────────────
// Малое простое поле: для теста сходимости ключей этого достаточно, а свойство
// ecdh(a_priv, b_pub) == ecdh(b_priv, a_pub) выполняется по-настоящему.
using u128 = unsigned __int128;
constexpr uint64_t P = 0xFFFFFFFFFFFFFFC5ull;   // простое, близкое к 2^64
constexpr uint64_t G = 5;

uint64_t mulmod(uint64_t a, uint64_t b) { return uint64_t((u128(a) * b) % P); }
uint64_t powmod(uint64_t b, uint64_t e) {
    uint64_t r = 1; b %= P;
    while (e) { if (e & 1) r = mulmod(r, b); b = mulmod(b, b); e >>= 1; }
    return r;
}
std::mt19937_64 rng(12345);

}  // namespace

namespace voile {

bool genKeyPair(KeyPair& out) {
    uint64_t priv = (rng() % (P - 3)) + 2;
    uint64_t pub  = powmod(G, priv);
    memset(out.priv, 0, sizeof(out.priv));
    memcpy(out.priv, &priv, 8);
    memset(out.pubComp, 0, sizeof(out.pubComp));
    out.pubComp[0] = 0x02;                   // признак сжатой точки
    memcpy(out.pubComp + 1, &pub, 8);
    return true;
}

bool ecdh(const uint8_t priv[32], const uint8_t peerPubComp[kPubComp],
          uint8_t out[kSharedLen]) {
    uint64_t a, b;
    memcpy(&a, priv, 8);
    memcpy(&b, peerPubComp + 1, 8);
    uint64_t shared = powmod(b, a);
    // Растягиваем до 32 байт хешем — как настоящий вывод из координаты X.
    uint8_t tmp[8];
    memcpy(tmp, &shared, 8);
    sha256(tmp, 8, out);
    return true;
}

void hkdf(const uint8_t* secret, size_t secretLen,
          const uint8_t* salt, size_t saltLen,
          const uint8_t* info, size_t infoLen,
          uint8_t* out, size_t outLen) {
    uint8_t zero[32] = {};
    uint8_t prk[32];
    hmacSha256(salt ? salt : zero, salt ? saltLen : 32, secret, secretLen, prk);
    uint8_t t[32]; size_t tlen = 0, done = 0; uint8_t ctr = 1;
    while (done < outLen) {
        std::vector<uint8_t> buf;
        buf.insert(buf.end(), t, t + tlen);
        if (info && infoLen) buf.insert(buf.end(), info, info + infoLen);
        buf.push_back(ctr++);
        hmacSha256(prk, 32, buf.data(), buf.size(), t);
        tlen = 32;
        size_t k = outLen - done; if (k > 32) k = 32;
        memcpy(out + done, t, k);
        done += k;
    }
}

// Шифрование в тесте не проверяется — только логика ключей, поэтому здесь заглушки,
// сохраняющие интерфейс.
bool seal(const uint8_t[kKeyLen], uint64_t, const uint8_t*, size_t,
          const uint8_t* plain, size_t plainLen, uint8_t* out, uint8_t tag[8]) {
    memcpy(out, plain, plainLen); memset(tag, 0xAA, 8); return true;
}
bool open(const uint8_t[kKeyLen], uint64_t, const uint8_t*, size_t,
          const uint8_t* cipher, size_t cipherLen, const uint8_t[8], uint8_t* out) {
    memcpy(out, cipher, cipherLen); return true;
}

}  // namespace voile
