#include "geometry_simd.h"
#include <stdint.h>
#include <string.h>

/* The kernels must round exactly as SSE does: no fast-math, no contraction, no reassociation. */
#if defined(__FAST_MATH__) || defined(_M_FP_FAST)
#error geometry_simd requires precise floating-point arithmetic
#endif
#if defined(__clang__)
#pragma clang fp contract(off) reassociate(off)
#elif defined(__GNUC__)
#pragma GCC optimize("fp-contract=off")
#elif defined(_MSC_VER)
#pragma float_control(precise, on, push)
#pragma fp_contract(off)
#endif

#if defined(__aarch64__)
#include <arm_neon.h>
#define GEOMETRY_NEON 1
#elif defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#include <xmmintrin.h>
#define GEOMETRY_SSE2 1
#else
#error geometry_simd requires ARM64 NEON or x86 SSE2
#endif

int geometry_simd_supported(void)
{
#if defined(GEOMETRY_NEON)
    uint64_t fpcr;
    __asm__ volatile("mrs %0, fpcr" : "=r"(fpcr));
    /* RMode, FZ, DN, exception enables, FZ16, FIZ, AH and NEP must all be clear. The newer
     * controls read as 0 (RES0) on CPUs without them. */
    const uint64_t incompatible =
        (3ull << 22) | (1ull << 24) | (1ull << 25) | (31ull << 8) | (1ull << 15) | (1ull << 19) | 7ull;
    return !(fpcr & incompatible);
#else
    /* MXCSR: every exception masked; no DAZ, FTZ or directed rounding. Status bits 0..5 are
     * ignored. */
    return (_mm_getcsr() & 0xffc0u) == 0x1f80u;
#endif
}

static uint32_t bits(float f) { uint32_t u; memcpy(&u, &f, sizeof u); return u; }
static float value(uint32_t u) { float f; memcpy(&f, &u, sizeof f); return f; }

/* One SSE operation, NaNs included. SSE returns the first operand's NaN when both are NaNs, quiets
 * signalling NaNs, and gives negative indefinite for an invalid operation on non-NaNs. NEON differs,
 * and C compilers may commute x86 operands, so the kernels redo a vertex this way only when a stored
 * XYZ lane is NaN. */
static float sse_arith(float a, float b, int multiply)
{
    uint32_t ua = bits(a), ub = bits(b);
    if ((ua & 0x7fffffffu) > 0x7f800000u)
        return value(ua | 0x00400000u);
    if ((ub & 0x7fffffffu) > 0x7f800000u)
        return value(ub | 0x00400000u);
    volatile float result = multiply ? a * b : a + b;
    uint32_t ur = bits(result);
    return (ur & 0x7fffffffu) > 0x7f800000u ? value(0xffc00000u) : result;
}

/* A whole vertex one SSE operation at a time, in the vector code's order: the NaN path, and every
 * vertex in the GEOMETRY_SIMD_SCALAR build that tests/geometry_simd_test.py compares against. */
static void exceptional(const float* a, const float* b, const float* s, float* p, float* n)
{
    for (unsigned k = 0; k < 3; ++k)
    {
        unsigned stride = b ? 2 : 1, normal = b ? 8 : 3;
        p[k] = sse_arith(s[0], a[k], 1);
        n[k] = sse_arith(s[normal], a[k], 1);
        for (unsigned j = 1; j < 3; ++j)
        {
            p[k] = sse_arith(p[k], sse_arith(s[j * stride], a[k + j * 4], 1), 0);
            n[k] = sse_arith(n[k], sse_arith(s[normal + j * stride], a[k + j * 4], 1), 0);
        }
        p[k] = sse_arith(p[k], b ? sse_arith(s[6], a[k + 12], 1) : a[k + 12], 0);
        if (b)
        {
            for (unsigned j = 0; j < 4; ++j)
                p[k] = sse_arith(p[k], sse_arith(s[1 + j * 2], b[k + j * 4], 1), 0);
            for (unsigned j = 0; j < 3; ++j)
                n[k] = sse_arith(n[k], sse_arith(s[9 + j * 2], b[k + j * 4], 1), 0);
        }
    }
}

#if !defined(GEOMETRY_SIMD_SCALAR)
#if defined(GEOMETRY_NEON)
typedef float32x4_t Vec;
static Vec load(const float* p) { return vld1q_f32(p); }
static Vec mul(Vec a, float b) { return vmulq_n_f32(a, b); }
static Vec add(Vec a, Vec b) { return vaddq_f32(a, b); }
static void store(float* p, Vec v) { vst1q_f32(p, v); }
#else
typedef __m128 Vec;
static Vec load(const float* p) { return _mm_loadu_ps(p); }
static Vec mul(Vec a, float b) { return _mm_mul_ps(a, _mm_set1_ps(b)); }
static Vec add(Vec a, Vec b) { return _mm_add_ps(a, b); }
static void store(float* p, Vec v) { _mm_storeu_ps(p, v); }
#endif

/* whether any stored XYZ lane of either output is a NaN */
static int nan3(const float* p, const float* n)
{
    for (unsigned k = 0; k < 3; ++k)
        if ((bits(p[k]) & 0x7fffffffu) > 0x7f800000u || (bits(n[k]) & 0x7fffffffu) > 0x7f800000u)
            return 1;
    return 0;
}
#endif

void geometry_simd_rigid(const void* matrix, const void* source, void* position, void* normal)
{
    float a[16], s[6], p[4], n[4];
    memcpy(a, matrix, sizeof a);
    memcpy(s, source, sizeof s);
#if !defined(GEOMETRY_SIMD_SCALAR)
    Vec a0 = load(a), a1 = load(a + 4), a2 = load(a + 8), a3 = load(a + 12);
    Vec pos = mul(a0, s[0]), norm = mul(a0, s[3]);
    pos = add(pos, mul(a1, s[1]));
    norm = add(norm, mul(a1, s[4]));
    pos = add(pos, mul(a2, s[2]));
    norm = add(norm, mul(a2, s[5]));
    pos = add(pos, a3);
    store(p, pos);
    store(n, norm);
    if (nan3(p, n))
#endif
        exceptional(a, NULL, s, p, n);
    memcpy(position, p, 12);
    memcpy(normal, n, 12);
}

void geometry_simd_weighted(const void* matrix_a, const void* matrix_b, const void* source, void* position,
                            void* normal)
{
    float a[16], b[16], s[14], p[4], n[4];
    memcpy(a, matrix_a, sizeof a);
    memcpy(b, matrix_b, sizeof b);
    memcpy(s, source, sizeof s);
#if !defined(GEOMETRY_SIMD_SCALAR)
    Vec a0 = load(a), a1 = load(a + 4), a2 = load(a + 8), a3 = load(a + 12);
    Vec b0 = load(b), b1 = load(b + 4), b2 = load(b + 8), b3 = load(b + 12);
    Vec pos = mul(a0, s[0]), norm = mul(a0, s[8]);
    pos = add(pos, mul(a1, s[2]));
    norm = add(norm, mul(a1, s[10]));
    pos = add(pos, mul(a2, s[4]));
    norm = add(norm, mul(a2, s[12]));
    pos = add(pos, mul(a3, s[6]));
    pos = add(pos, mul(b0, s[1]));
    norm = add(norm, mul(b0, s[9]));
    pos = add(pos, mul(b1, s[3]));
    norm = add(norm, mul(b1, s[11]));
    pos = add(pos, mul(b2, s[5]));
    norm = add(norm, mul(b2, s[13]));
    pos = add(pos, mul(b3, s[7]));
    store(p, pos);
    store(n, norm);
    if (nan3(p, n))
#endif
        exceptional(a, b, s, p, n);
    memcpy(position, p, 12);
    memcpy(normal, n, 12);
}

#if defined(_MSC_VER) && !defined(__clang__)
#pragma float_control(pop)
#endif
