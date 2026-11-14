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
#include <emmintrin.h>
#define GEOMETRY_SSE2 1
#elif defined(__EMSCRIPTEN__)
#define GEOMETRY_NONE 1 /* the browser: geometry_guest.c keeps the kernels off */
#else
#error geometry_simd requires ARM64 NEON or x86 SSE2
#endif

#if defined(GEOMETRY_NONE)
int geometry_simd_supported(void) { return 0; }
void geometry_simd_rigid(const void* matrix, const void* source, void* position, void* normal)
{
    (void)matrix, (void)source, (void)position, (void)normal;
}
void geometry_simd_weighted(const void* matrix_a, const void* matrix_b, const void* source, void* position, void* normal)
{
    (void)matrix_a, (void)matrix_b, (void)source, (void)position, (void)normal;
}
#else

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

/* The vector kernels read their inputs where the caller keeps them and write each output's 12 bytes
 * directly. (Copying the inputs to the stack first let clang reload the copies with 16-byte loads that
 * straddle two of its own stores, which stalls store-to-load forwarding on every vertex.) Every input is
 * read before the first output byte is written, so the outputs may alias the inputs. */
#if !defined(GEOMETRY_SIMD_SCALAR)
#if defined(GEOMETRY_NEON)
typedef float32x4_t Vec;
static Vec load(const unsigned char* p) { return vreinterpretq_f32_u8(vld1q_u8(p)); }
static Vec mul(Vec a, float b) { return vmulq_n_f32(a, b); }
static Vec add(Vec a, Vec b) { return vaddq_f32(a, b); }
static void store3(unsigned char* p, Vec v)
{
    float z = vgetq_lane_f32(v, 2);
    vst1_u8(p, vreinterpret_u8_f32(vget_low_f32(v)));
    memcpy(p + 8, &z, sizeof z);
}
/* whether lane 0, 1 or 2 of either vector is a NaN */
static int nan3(Vec p, Vec n)
{
    uint32x4_t bad = vmvnq_u32(vandq_u32(vceqq_f32(p, p), vceqq_f32(n, n)));
    return vmaxvq_u32(vsetq_lane_u32(0, bad, 3)) != 0;
}
#else
typedef __m128 Vec;
static Vec load(const unsigned char* p) { return _mm_loadu_ps((const float*)p); }
static Vec mul(Vec a, float b) { return _mm_mul_ps(a, _mm_set1_ps(b)); }
static Vec add(Vec a, Vec b) { return _mm_add_ps(a, b); }
static void store3(unsigned char* p, Vec v)
{
    float z = _mm_cvtss_f32(_mm_movehl_ps(v, v));
    _mm_storel_epi64((__m128i*)p, _mm_castps_si128(v));
    memcpy(p + 8, &z, sizeof z);
}
static int nan3(Vec p, Vec n)
{
    return (_mm_movemask_ps(_mm_or_ps(_mm_cmpunord_ps(p, p), _mm_cmpunord_ps(n, n))) & 7) != 0;
}
#endif

/* source float k */
static float at(const unsigned char* s, unsigned k)
{
    float f;
    memcpy(&f, s + 4 * k, sizeof f);
    return f;
}
#endif

void geometry_simd_rigid(const void* matrix, const void* source, void* position, void* normal)
{
#if !defined(GEOMETRY_SIMD_SCALAR)
    const unsigned char *m = (const unsigned char*)matrix, *src = (const unsigned char*)source;
    Vec a0 = load(m), a1 = load(m + 16), a2 = load(m + 32), a3 = load(m + 48);
    Vec pos = mul(a0, at(src, 0)), norm = mul(a0, at(src, 3));
    pos = add(pos, mul(a1, at(src, 1)));
    norm = add(norm, mul(a1, at(src, 4)));
    pos = add(pos, mul(a2, at(src, 2)));
    norm = add(norm, mul(a2, at(src, 5)));
    pos = add(pos, a3);
    if (!nan3(pos, norm))
    {
        store3((unsigned char*)position, pos);
        store3((unsigned char*)normal, norm);
        return;
    }
#endif
    float a[16], s[6], p[4], n[4];
    memcpy(a, matrix, sizeof a);
    memcpy(s, source, sizeof s);
    exceptional(a, NULL, s, p, n);
    memcpy(position, p, 12);
    memcpy(normal, n, 12);
}

void geometry_simd_weighted(const void* matrix_a, const void* matrix_b, const void* source, void* position,
                            void* normal)
{
#if !defined(GEOMETRY_SIMD_SCALAR)
    const unsigned char *ma = (const unsigned char*)matrix_a, *mb = (const unsigned char*)matrix_b;
    const unsigned char* src = (const unsigned char*)source;
    Vec a0 = load(ma), a1 = load(ma + 16), a2 = load(ma + 32), a3 = load(ma + 48);
    Vec b0 = load(mb), b1 = load(mb + 16), b2 = load(mb + 32), b3 = load(mb + 48);
    Vec pos = mul(a0, at(src, 0)), norm = mul(a0, at(src, 8));
    pos = add(pos, mul(a1, at(src, 2)));
    norm = add(norm, mul(a1, at(src, 10)));
    pos = add(pos, mul(a2, at(src, 4)));
    norm = add(norm, mul(a2, at(src, 12)));
    pos = add(pos, mul(a3, at(src, 6)));
    pos = add(pos, mul(b0, at(src, 1)));
    norm = add(norm, mul(b0, at(src, 9)));
    pos = add(pos, mul(b1, at(src, 3)));
    norm = add(norm, mul(b1, at(src, 11)));
    pos = add(pos, mul(b2, at(src, 5)));
    norm = add(norm, mul(b2, at(src, 13)));
    pos = add(pos, mul(b3, at(src, 7)));
    if (!nan3(pos, norm))
    {
        store3((unsigned char*)position, pos);
        store3((unsigned char*)normal, norm);
        return;
    }
#endif
    float a[16], b[16], s[14], p[4], n[4];
    memcpy(a, matrix_a, sizeof a);
    memcpy(b, matrix_b, sizeof b);
    memcpy(s, source, sizeof s);
    exceptional(a, b, s, p, n);
    memcpy(position, p, 12);
    memcpy(normal, n, 12);
}

#if defined(_MSC_VER) && !defined(__clang__)
#pragma float_control(pop)
#endif
#endif /* GEOMETRY_NONE */
