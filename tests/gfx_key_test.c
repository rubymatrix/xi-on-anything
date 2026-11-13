/* runtime/portable/gfx_key.h, the back ends' map hash and compare, without a GPU: the hash is nonzero and
 * depends on every byte and the length, single-bit changes reach the bucket index (the low bits), keys that
 * differ only in the high half of a word spread over buckets, nothing reads past the key (a key ending at an
 * inaccessible page), and gfx_key_eq agrees with memcmp at every length, offset and differing byte.
 *
 * usage: python3 tools/build_posix.py keytest (exit status 0 when every check passes) */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "gfx_key.h"

static unsigned checks, errors;

static void check(int ok, const char* what, size_t n, size_t at)
{
    checks++;
    if (!ok && errors++ < 20)
        printf("FAIL %s (length %zu, byte %zu)\n", what, n, at);
}

static uint64_t rng = 0x243f6a8885a308d3ull;
static uint8_t rand8(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return (uint8_t)(rng >> 24);
}

/* the back ends' key sizes (sampler, LibKey, PipeKey, CacheKey and the kept-token pair) and odd ones */
static const size_t sizes[] = {1, 3, 7, 8, 9, 15, 16, 17, 24, 31, 32, 33, 40, 63, 64, 65, 113, 116, 224, 248, 320};

int main(void)
{
    uint8_t key[512], other[512];

    /* every single-bit flip changes the hash, and nearly always its low 12 bits */
    for (size_t s = 0; s < sizeof sizes / sizeof *sizes; ++s)
    {
        size_t n = sizes[s];
        for (size_t i = 0; i < n; ++i)
            key[i] = rand8();
        uint64_t h = gfx_key_hash(key, n);
        check(h != 0 && h == gfx_key_hash(key, n), "nonzero and deterministic", n, 0);
        unsigned flips = 0, low = 0;
        for (size_t i = 0; i < n; ++i)
            for (int bit = 0; bit < 8; ++bit)
            {
                memcpy(other, key, n);
                other[i] ^= (uint8_t)(1 << bit);
                uint64_t g = gfx_key_hash(other, n);
                check(g != h, "a single-bit flip changes the hash", n, i);
                flips++;
                low += ((g ^ h) & 0xfff) != 0;
            }
        check(low * 100 >= flips * 99, "single-bit flips reach the low 12 bits", n, 0);
        /* the length is hashed: a key and the same bytes plus a zero byte differ */
        memcpy(other, key, n);
        other[n] = 0;
        check(gfx_key_hash(other, n + 1) != h, "the length changes the hash", n, n);
    }

    /* keys that differ only in bytes 4-7 of one word (as PipeKey's depth and x8/topo/fill do) spread over
     * 4096 buckets about as random ones would (~2589 used) */
    {
        static uint8_t used[4096];
        unsigned distinct = 0;
        memset(key, 0, 248);
        for (uint32_t v = 0; v < 4096; ++v)
        {
            memcpy(key + 29 * 8 + 4, &v, 4);
            uint32_t b = (uint32_t)gfx_key_hash(key, 248) & 4095;
            distinct += !used[b];
            used[b] = 1;
        }
        check(distinct >= 2400, "high-half differences spread over buckets", 248, 29 * 8 + 4);
    }

    /* gfx_key_eq is !memcmp at every length 0-80, source offset 0-7 and differing byte */
    for (size_t n = 0; n <= 80; ++n)
        for (size_t off = 0; off < 8; ++off)
        {
            uint8_t* a = key + off;
            uint8_t* b = other + (7 - off);
            for (size_t i = 0; i < n; ++i)
                a[i] = b[i] = rand8();
            check(gfx_key_eq(a, b, n), "equal keys compare equal", n, 0);
            for (size_t i = 0; i < n; ++i)
            {
                uint8_t was = b[i];
                b[i] ^= (uint8_t)(1 + rand8() % 255);
                check(!gfx_key_eq(a, b, n) && memcmp(a, b, n) != 0, "a differing byte compares unequal", n, i);
                b[i] = was;
            }
        }

    /* nothing reads past the key: each key ends where an inaccessible page starts */
    {
        long page = sysconf(_SC_PAGESIZE);
        uint8_t* map =
            (uint8_t*)mmap(NULL, (size_t)page * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (map == MAP_FAILED || mprotect(map + page, (size_t)page, PROT_NONE))
        {
            printf("FAIL mmap\n");
            return 1;
        }
        for (size_t n = 0; n <= 330; ++n)
        {
            uint8_t* k = map + page - n;
            memset(k, 0x5a, n);
            memset(other, 0x5a, n);
            check(gfx_key_hash(k, n) == gfx_key_hash(other, n), "hash independent of the key's address", n, 0);
            check(gfx_key_eq(k, other, n) && gfx_key_eq(other, k, n), "compare at a page end", n, 0);
        }
        munmap(map, (size_t)page * 2);
    }

    printf("%u checks, %u errors\n", checks, errors);
    return errors != 0;
}
