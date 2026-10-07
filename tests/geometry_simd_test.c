/* Synthetic, game-free differential tests. No captured game data or generated C.
 * x86 GCC/Clang uses hardware scalar SSE as the oracle; ARM uses double
 * intermediates rounded to binary32, with explicit IEEE special values. */
#include "geometry_simd.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__SSE2__) || defined(_M_X64)
#include <xmmintrin.h>
#endif

static unsigned checks, ignored_controls;
static uint32_t random_state = 0x7b10ac35u;
static uint64_t digest = UINT64_C(14695981039346656037);

static void require(int ok, const char* what)
{
    ++checks;
    if (!ok)
    {
        fprintf(stderr, "FAIL: %s (check %u)\n", what, checks);
        exit(1);
    }
}

static uint32_t bits(float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

static float value(uint32_t u)
{
    float f;
    memcpy(&f, &u, 4);
    return f;
}

static uint32_t random_bits(void)
{
    random_state ^= random_state << 13;
    random_state ^= random_state >> 17;
    random_state ^= random_state << 5;
    return random_state;
}

static float reference_op(float a, float b, int multiply)
{
#if (defined(__x86_64__) || defined(__i386__)) && (defined(__GNUC__) || defined(__clang__))
    /* Explicit destination/source order even when both inputs are NaNs. */
    if (multiply)
        __asm__ volatile("mulss %1, %0" : "+x"(a) : "xm"(b));
    else
        __asm__ volatile("addss %1, %0" : "+x"(a) : "xm"(b));
    return a;
#else
    uint32_t ua = bits(a), ub = bits(b);
    if ((ua & 0x7fffffffu) > 0x7f800000u)
        return value(ua | 0x00400000u);
    if ((ub & 0x7fffffffu) > 0x7f800000u)
        return value(ub | 0x00400000u);
    volatile double wide = multiply ? (double)a * (double)b : (double)a + (double)b;
    volatile float rounded = (float)wide;
    return (bits(rounded) & 0x7fffffffu) > 0x7f800000u ? value(0xffc00000u) : rounded;
#endif
}

static void reference(const float* a, const float* b, const float* s, float* p, float* n)
{
    /* Per-lane, sequential terms, independent of the production vector code. */
    for (unsigned lane = 0; lane < 3; ++lane)
    {
        unsigned terms = b ? 8 : 4;
        float position = 0, normal = 0;
        for (unsigned term = 0; term < terms; ++term)
        {
            unsigned col = term % 4;
            const float* matrix = term < 4 ? a : b;
            unsigned index = b ? col * 2 + (term >= 4) : col;
            float product = !b && col == 3 ? a[12 + lane] : reference_op(s[index], matrix[4 * col + lane], 1);
            position = term ? reference_op(position, product, 0) : product;
            if (col != 3)
            {
                unsigned ni = b ? index + 8 : index + 3;
                product = reference_op(s[ni], matrix[4 * col + lane], 1);
                normal = term ? reference_op(normal, product, 0) : product;
            }
        }
        p[lane] = position;
        n[lane] = normal;
    }
}

static void hash_bytes(const void* data, size_t size)
{
    const unsigned char* p = data;
    for (size_t i = 0; i < size; ++i)
    {
        digest ^= p[i];
        digest *= UINT64_C(1099511628211);
    }
}

static void check_case(const float* a, const float* b, const float* s, unsigned layout, unsigned alignment)
{
    unsigned char actual[640], expected[640];
    size_t ma = 16 + alignment, mb = 128 + alignment, src = 240 + alignment;
    size_t po = 368 + alignment, no = 416 + alignment;
    /* Both kernels must snapshot before any store, including exceptional NaNs.
     * Include partial output overlap, exact overlap, matrix/source overlap and
     * both outputs overwriting distinct parts of one input. */
    switch (layout)
    {
    case 1:
        po = ma;
        break;
    case 2:
        no = ma + 4;
        break;
    case 3:
        po = src;
        no = src + 8;
        break;
    case 4:
        no = po;
        break;
    case 5:
        no = po + 4;
        break;
    case 6:
        po = no + 4;
        break;
    case 7:
        po = mb + 8;
        no = ma + 48;
        break;
    case 8:
        po = ma + 1;
        no = src + 3;
        break;
    case 9:
        mb = ma;
        break;
    default:
        break;
    }
    memset(actual, 0xa5, sizeof actual);
    memcpy(actual + ma, a, 64);
    if (b)
        memcpy(actual + mb, b, 64);
    memcpy(actual + src, s, b ? 56 : 24);
    memcpy(expected, actual, sizeof actual);
    float aa[16], bb[16], ss[14], p[3], n[3];
    memcpy(aa, actual + ma, 64);
    if (b)
        memcpy(bb, actual + mb, 64);
    memcpy(ss, actual + src, b ? 56 : 24);
    reference(aa, b ? bb : NULL, ss, p, n);
    memcpy(expected + po, p, 12);
    memcpy(expected + no, n, 12);
    if (b)
        geometry_simd_weighted(actual + ma, actual + mb, actual + src, actual + po, actual + no);
    else
        geometry_simd_rigid(actual + ma, actual + src, actual + po, actual + no);
    if (memcmp(actual, expected, sizeof actual))
        fprintf(stderr, "kernel=%s layout=%u alignment=%u\n", b ? "weighted" : "rigid", layout, alignment);
    require(!memcmp(actual, expected, sizeof actual), "output bytes and surrounding memory");
    hash_bytes(actual, sizeof actual);
}

static void golden_cases(void)
{
    float a[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 10, 20, 30, 1};
    float b[16] = {2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 2, 0, -2, -4, -6, 1};
    float s[14] = {1, 2, 3, 4, 5, 6}, p[3], n[3];
    const float rp[] = {11, 22, 33}, rn[] = {4, 5, 6};
    geometry_simd_rigid(a, s, p, n);
    require(!memcmp(p, rp, 12) && !memcmp(n, rn, 12), "rigid affine position and untranslated normal");
    const float weighted[] = {1, 2, 3, 4, 5, 6, 0.5f, 0.25f, 7, 8, 9, 10, 11, 12};
    const float wp[] = {9.5f, 20, 30.5f}, wn[] = {23, 29, 35};
    geometry_simd_weighted(a, b, weighted, p, n);
    require(!memcmp(p, wp, 12) && !memcmp(n, wn, 12), "weighted terms and translation weights");

    /* Product rounds to 1 before cancellation: FMA would produce -2^-46. */
    memset(a, 0, sizeof a);
    a[0] = value(0x3f800001u);
    a[4] = -1;
    s[0] = value(0x3f7ffffeu);
    s[1] = 1;
    s[2] = 0;
    geometry_simd_rigid(a, s, p, n);
    require(bits(p[0]) == 0, "multiply/add must not contract");

    /* ((2^24 + 1) + -2^24) + 0 = 0, not 1. */
    a[0] = 16777216;
    a[4] = 1;
    a[8] = -16777216;
    s[0] = s[1] = s[2] = 1;
    geometry_simd_rigid(a, s, p, n);
    require(bits(p[0]) == 0, "addition must not reassociate");

    /* Minimum subnormal survives; all-negative-zero terms keep their sign. */
    memset(a, 0, sizeof a);
    memset(s, 0, sizeof s);
    a[0] = 1;
    s[0] = value(1);
    geometry_simd_rigid(a, s, p, n);
    require(bits(p[0]) == 1, "gradual underflow");
    for (unsigned j = 0; j < 16; ++j)
        a[j] = value(0x80000000u);
    for (unsigned j = 0; j < 6; ++j)
        s[j] = 1;
    geometry_simd_rigid(a, s, p, n);
    require(bits(p[0]) == 0x80000000u && bits(n[0]) == 0x80000000u, "negative zero");
}

static void environment_cases(void)
{
#if defined(__aarch64__)
    uint64_t saved;
    __asm__ volatile("mrs %0, fpcr" : "=r"(saved));
    const uint64_t mask =
        (3ull << 22) | (1ull << 24) | (1ull << 25) | (31ull << 8) | (1ull << 15) | (1ull << 19) | 7ull;
    uint64_t canonical = saved & ~mask;
    __asm__ volatile("msr fpcr, %0" ::"r"(canonical) : "memory");
    require(geometry_simd_supported(), "default ARM FP environment");
    const unsigned controls[] = {0, 1, 2, 8, 9, 10, 11, 12, 15, 19, 22, 23, 24, 25};
    for (unsigned i = 0; i < sizeof controls / sizeof controls[0]; ++i)
    {
        uint64_t altered = canonical | (1ull << controls[i]), read;
        __asm__ volatile("msr fpcr, %0" ::"r"(altered) : "memory");
        __asm__ volatile("mrs %0, fpcr" : "=r"(read));
        if (read == canonical)
            ++ignored_controls;
        int supported = geometry_simd_supported();
        uint64_t after;
        __asm__ volatile("mrs %0, fpcr" : "=r"(after));
        __asm__ volatile("msr fpcr, %0" ::"r"(canonical) : "memory");
        require(read == after, "admission preserves ARM FP control");
        require(supported == !(read & mask), "unsupported ARM control (including RES0)");
    }
    __asm__ volatile("msr fpcr, %0" ::"r"(saved) : "memory");
#elif defined(__SSE2__) || defined(_M_X64)
    unsigned saved = _mm_getcsr();
    _mm_setcsr(0x1f80u);
    require(geometry_simd_supported(), "default SSE FP environment");
    const unsigned controls[] = {6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    for (unsigned i = 0; i < sizeof controls / sizeof controls[0]; ++i)
    {
        unsigned altered = 0x1f80u ^ (1u << controls[i]);
        _mm_setcsr(altered);
        unsigned read = _mm_getcsr();
        if (read == 0x1f80u)
            ++ignored_controls; /* Rosetta can ignore exception-unmask requests. */
        int supported = geometry_simd_supported();
        unsigned after = _mm_getcsr();
        _mm_setcsr(0x1f80u);
        require(supported == ((read & 0xffc0u) == 0x1f80u), "actual SSE control admitted or rejected");
        require(after == read, "admission preserves SSE FP control");
    }
    _mm_setcsr(0x1fbfu);
    require(geometry_simd_supported(), "sticky SSE status is not a control change");
    _mm_setcsr(saved);
#endif
    require(geometry_simd_supported(), "test launch environment admitted");
}

int main(void)
{
    environment_cases();
    golden_cases();
    const uint32_t special[] = {0,           0x80000000u, 1,           0x80000001u, 0x007fffffu, 0x00800000u,
                                0x7f7fffffu, 0xff7fffffu, 0x7f800000u, 0xff800000u, 0x7fc12345u, 0xffc54321u,
                                0x7f812345u, 0xff854321u, 0x3f800000u, 0xbf800000u};
    float a[16], b[16], s[14];
    for (unsigned test = 0; test < 4096; ++test)
    {
        for (unsigned j = 0; j < 46; ++j)
        {
            uint32_t u = test < 256 ? special[(test + j) % 16] : random_bits();
            if (test >= 1024)
                u = (u & 0x807fffffu) | ((80u + random_bits() % 90u) << 23);
            float f = value(u);
            if (j < 16)
                a[j] = f;
            else if (j < 32)
                b[j - 16] = f;
            else
                s[j - 32] = f;
        }
        for (unsigned layout = 0; layout < 10; ++layout)
        {
            check_case(a, NULL, s, layout, test % 16);
            check_case(a, b, s, layout, test % 16);
        }
    }
    /* Every pairing of signed quiet/signalling NaNs, infinity and finite
     * special values in both operand positions; do not rely on random NaNs. */
    for (unsigned x = 0; x < 16; ++x)
        for (unsigned y = 0; y < 16; ++y)
        {
            for (unsigned j = 0; j < 16; ++j)
                a[j] = value(special[x]), b[j] = value(special[y]);
            for (unsigned j = 0; j < 14; ++j)
                s[j] = value(special[y]);
            check_case(a, NULL, s, 3, 1);
            check_case(a, b, s, 7, 3);
        }
    printf("{\"checks\":%u,\"mismatches\":0,\"ignored_controls\":%u,\"digest\":\"%016llx\"}\n", checks,
           ignored_controls, (unsigned long long)digest);
    return 0;
}
