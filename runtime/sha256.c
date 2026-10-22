/* SHA-256 (FIPS 180-4), written for the stack (MIT, like the repo). */
#include <string.h>

#include "sha256.h"

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

/* The first 32 bits of the fractional parts of the cube roots of the first 64 primes (FIPS 180-4 4.2.2). */
static const uint32_t port_sha256_k[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

static void port_sha256_block(PortSha256 *c, const uint8_t *p) {
    uint32_t w[64], a, b, cc, d, e, f, g, h, t1, t2;
    int i;
    for (i = 0; i < 16; i++) {
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    }
    for (; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = c->h[0];
    b = c->h[1];
    cc = c->h[2];
    d = c->h[3];
    e = c->h[4];
    f = c->h[5];
    g = c->h[6];
    h = c->h[7];
    for (i = 0; i < 64; i++) {
        t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + port_sha256_k[i] + w[i];
        t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & cc) ^ (b & cc));
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = cc;
        cc = b;
        b = a;
        a = t1 + t2;
    }
    c->h[0] += a;
    c->h[1] += b;
    c->h[2] += cc;
    c->h[3] += d;
    c->h[4] += e;
    c->h[5] += f;
    c->h[6] += g;
    c->h[7] += h;
}

void port_sha256_init(PortSha256 *c) {
    /* The first 32 bits of the fractional parts of the square roots of the first 8 primes (FIPS 180-4 5.3.3). */
    static const uint32_t h0[8] = { 0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                                    0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u };
    memcpy(c->h, h0, sizeof(h0));
    c->length = 0;
    c->used = 0;
}

void port_sha256_update(PortSha256 *c, const void *data, size_t n) {
    const uint8_t *p = data;
    c->length += n;
    if (c->used > 0) {
        size_t take = 64 - c->used < n ? 64 - c->used : n;
        memcpy(c->block + c->used, p, take);
        c->used += take;
        p += take;
        n -= take;
        if (c->used < 64) {
            return;
        }
        port_sha256_block(c, c->block);
        c->used = 0;
    }
    for (; n >= 64; p += 64, n -= 64) {
        port_sha256_block(c, p);
    }
    memcpy(c->block, p, n);
    c->used = n;
}

void port_sha256_final(PortSha256 *c, uint8_t digest[32]) {
    uint64_t bits = c->length * 8;
    uint8_t pad[72];
    size_t padlen = (c->used < 56 ? 56 : 120) - c->used;
    int i;
    memset(pad, 0, sizeof(pad));
    pad[0] = 0x80;
    for (i = 0; i < 8; i++) {
        pad[padlen + i] = (uint8_t)(bits >> (56 - 8 * i));
    }
    port_sha256_update(c, pad, padlen + 8);
    for (i = 0; i < 32; i++) {
        digest[i] = (uint8_t)(c->h[i / 4] >> (24 - 8 * (i % 4)));
    }
}

void port_sha256(const void *data, size_t n, uint8_t digest[32]) {
    PortSha256 c;
    port_sha256_init(&c);
    port_sha256_update(&c, data, n);
    port_sha256_final(&c, digest);
}

void port_sha256_hex(const uint8_t digest[32], char hex[65]) {
    static const char digits[] = "0123456789abcdef";
    int i;
    for (i = 0; i < 32; i++) {
        hex[2 * i] = digits[digest[i] >> 4];
        hex[2 * i + 1] = digits[digest[i] & 0xF];
    }
    hex[64] = '\0';
}
