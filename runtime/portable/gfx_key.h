/* The back ends' small hash maps (pipelines, samplers, the shadow cache) look up a key of raw bytes on every
 * draw: hash and compare it 8 bytes at a time, not byte by byte. Both read exactly n bytes (through memcpy, so
 * any alignment) and treat every byte alike, so equal keys hash alike and the compare alone decides identity,
 * as with the byte-wise FNV-1a and memcmp these replace. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* one word into a lane: a multiply carries bits only upward, so without the rotate a difference in a word's top
 * bit (a float's sign) would stay in the lane's top bit, and two of them in one lane would cancel */
static inline uint64_t gfx_key_mix(uint64_t lane, uint64_t word, uint64_t k)
{
    lane = (lane ^ word) * k;
    return lane << 31 | lane >> 33;
}

/* four lanes of xor-multiply-rotate, so the multiplies overlap, then fmix64: the maps index buckets by the low
 * bits, so the finaliser folds the high half of every lane into them. Never 0: 0 marks an empty slot. */
static inline uint64_t gfx_key_hash(const void* p, size_t n)
{
    const uint8_t* b = (const uint8_t*)p;
    const uint64_t k0 = 0x9e3779b97f4a7c15ull, k1 = 0xc2b2ae3d27d4eb4full, k2 = 0x165667b19e3779f9ull,
                   k3 = 0xff51afd7ed558ccdull;
    uint64_t a = k0, c = k1, d = k2, e = k3, w0, w1, w2, w3;
    size_t i = 0;
    for (; i + 32 <= n; i += 32)
    {
        memcpy(&w0, b + i, 8);
        memcpy(&w1, b + i + 8, 8);
        memcpy(&w2, b + i + 16, 8);
        memcpy(&w3, b + i + 24, 8);
        a = gfx_key_mix(a, w0, k1);
        c = gfx_key_mix(c, w1, k2);
        d = gfx_key_mix(d, w2, k3);
        e = gfx_key_mix(e, w3, k0);
    }
    for (; i + 8 <= n; i += 8)
    {
        memcpy(&w0, b + i, 8);
        a = gfx_key_mix(a, w0, k1);
    }
    if (i < n)
    {
        w0 = 0;
        memcpy(&w0, b + i, n - i);
        a = gfx_key_mix(a, w0, k1);
    }
    uint64_t h = a ^ c * k0 ^ d * k1 ^ e * k2 ^ n;
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdull;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ull;
    h ^= h >> 33;
    return h ? h : 1;
}

/* !memcmp(x, y, n), inline: on Linux the kit's compiler_rt memcmp is a byte loop */
static inline int gfx_key_eq(const void* x, const void* y, size_t n)
{
    const uint8_t* a = (const uint8_t*)x;
    const uint8_t* b = (const uint8_t*)y;
    uint64_t diff = 0, u, v;
    size_t i = 0;
    for (; i + 8 <= n; i += 8)
    {
        memcpy(&u, a + i, 8);
        memcpy(&v, b + i, 8);
        diff |= u ^ v;
    }
    for (; i < n; ++i)
        diff |= (uint64_t)(a[i] ^ b[i]);
    return !diff;
}
