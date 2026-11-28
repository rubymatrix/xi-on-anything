/* Runtime for generated code: indirect-call dispatch, traps, CPU identification. */
#include "runtime.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#if defined(__aarch64__)
#include <arm_neon.h>
#endif

/* rt_table (table.c, generated): every translated function, sorted by guest address. */
int rt_index(uint32_t addr)
{
    unsigned lo = 0, hi = rt_table_count;
    while (lo < hi)
    {
        unsigned mid = (lo + hi) / 2;
        if (rt_table[mid].addr < addr)
            lo = mid + 1;
        else
            hi = mid;
    }
    return (lo < rt_table_count && rt_table[lo].addr == addr) ? (int)lo : -1;
}

GuestFn rt_lookup(uint32_t addr)
{
    int i = rt_index(addr);
    return i >= 0 ? rt_table[i].fn : NULL;
}

static RtNativeHandler g_native = NULL;

void rt_set_native_handler(RtNativeHandler h) { g_native = h; }

uint32_t rt_reloc_delta;
int rt_native_check = -1; /* native_skin.h: FFXI_SKIN_CHECK, read at the first native loop */
uint32_t rt_image_lo, rt_image_hi;

/* native_skin.h: the order each row's four products are added in, by kind */
static const uint8_t mat_order[3][4][4] = {
    { { 2, 1, 0, 3 }, { 1, 2, 0, 3 }, { 2, 0, 3, 1 }, { 0, 3, 2, 1 } }, /* this_arg */
    { { 2, 1, 3, 0 }, { 0, 2, 3, 1 }, { 3, 2, 1, 0 }, { 3, 1, 2, 0 } }, /* arg_this */
    { { 0, 1, 2, 3 }, { 0, 1, 2, 3 }, { 0, 1, 2, 3 }, { 0, 1, 2, 3 } }, /* stdcall: from 0.0 */
};

void rt_mat_mul_c(unsigned char* gb, unsigned cw, uint32_t out, uint32_t l, uint32_t r, int kind)
{
    float m[16];
    for (uint32_t i = 0; i < 4; ++i)
        for (uint32_t j = 0; j < 4; ++j)
        {
            double s = 0.0;
            for (int k = 0; k < 4; ++k)
            {
                uint32_t e = mat_order[kind][i][k];
                double p = fr_cw(cw, (double)rdf32_b(gb, l + 16u * i + 4u * e) * rdf32_b(gb, r + 16u * e + 4u * j));
                s = k || kind == RT_MAT_STDCALL ? fr_cw(cw, s + p) : p;
            }
            m[4 * i + j] = (float)s;
        }
    for (uint32_t k = 0; k < 16; ++k)
        wrf32_b(gb, out + 4u * k, m[k]);
}

#if defined(__aarch64__)
/* (both halves of a row of r, times an element of l) */
#define MAT_FIRST(e) lo = vmulq_n_f64(rl[e], m[e]), hi = vmulq_n_f64(rh[e], m[e])
#define MAT_ADD(e) lo = vfmaq_n_f64(lo, rl[e], m[e]), hi = vfmaq_n_f64(hi, rh[e], m[e])
#define MAT_ZERO(e) lo = hi = vdupq_n_f64(0.0), MAT_ADD(e)
#define MAT_ROW(i, first, a, b, c, d)                                                                                  \
    do                                                                                                                 \
    {                                                                                                                  \
        double m[4];                                                                                                   \
        for (uint32_t k = 0; k < 4; ++k)                                                                               \
            m[k] = rdf32_b(gb, l + 16u * (i) + 4u * k);                                                                \
        first(a), MAT_ADD(b), MAT_ADD(c), MAT_ADD(d);                                                                  \
        res[i] = vcvt_high_f32_f64(vcvt_f32_f64(lo), hi);                                                              \
    } while (0)
#endif

void rt_mat_mul(unsigned char* gb, unsigned cw, uint32_t out, uint32_t l, uint32_t r, int kind)
{
#if defined(__aarch64__)
    if ((cw >> 8) & 3u)
    {
        float64x2_t rl[4], rh[4], lo, hi;
        float32x4_t res[4];
        for (uint32_t k = 0; k < 4; ++k)
        {
            float32x4_t v = vld1q_f32((const float*)(gb + (uint32_t)(r + 16u * k)));
            rl[k] = vcvt_f64_f32(vget_low_f32(v)), rh[k] = vcvt_high_f64_f32(v);
        }
        if (kind == RT_MAT_THIS_ARG)
        {
            MAT_ROW(0, MAT_FIRST, 2, 1, 0, 3);
            MAT_ROW(1, MAT_FIRST, 1, 2, 0, 3);
            MAT_ROW(2, MAT_FIRST, 2, 0, 3, 1);
            MAT_ROW(3, MAT_FIRST, 0, 3, 2, 1);
        }
        else if (kind == RT_MAT_ARG_THIS)
        {
            MAT_ROW(0, MAT_FIRST, 2, 1, 3, 0);
            MAT_ROW(1, MAT_FIRST, 0, 2, 3, 1);
            MAT_ROW(2, MAT_FIRST, 3, 2, 1, 0);
            MAT_ROW(3, MAT_FIRST, 3, 1, 2, 0);
        }
        else
        {
            MAT_ROW(0, MAT_ZERO, 0, 1, 2, 3);
            MAT_ROW(1, MAT_ZERO, 0, 1, 2, 3);
            MAT_ROW(2, MAT_ZERO, 0, 1, 2, 3);
            MAT_ROW(3, MAT_ZERO, 0, 1, 2, 3);
        }
        for (uint32_t k = 0; k < 4; ++k)
            vst1q_f32((float*)(gb + (uint32_t)(out + 16u * k)), res[k]);
        return;
    }
#endif
    rt_mat_mul_c(gb, cw, out, l, r, kind);
}

static volatile uint32_t g_lookup_gen = 1; /* rt_lookup_any's cache: bumped when code is placed */

void rt_set_image(uint32_t runtime_base)
{
    g_lookup_gen++;
    rt_reloc_delta = runtime_base - rt_image_base;
    rt_image_lo = runtime_base;
    rt_image_hi = runtime_base + rt_image_size;
}

typedef struct AddedModule
{
    const RtModule* m;
    uint32_t lo, hi;
} AddedModule;

static AddedModule g_modules[8];
static unsigned g_nmodules;

void rt_add_module(const RtModule* m, uint32_t runtime_base)
{
    if (g_nmodules < sizeof g_modules / sizeof g_modules[0])
    {
        g_lookup_gen++;
        *m->delta = runtime_base - m->base;
        g_modules[g_nmodules].m = m;
        g_modules[g_nmodules].lo = runtime_base;
        g_modules[g_nmodules].hi = runtime_base + m->size;
        g_nmodules++;
    }
}

static GuestFn module_lookup(const RtModule* m, uint32_t addr)
{
    unsigned lo = 0, hi = m->count;
    while (lo < hi)
    {
        unsigned mid = (lo + hi) / 2;
        if (m->table[mid].addr < addr)
            lo = mid + 1;
        else
            hi = mid;
    }
    return (lo < m->count && m->table[lo].addr == addr) ? m->table[lo].fn : NULL;
}

static GuestFn lookup_uncached(uint32_t target)
{
    /* Only addresses inside an image can be translated functions; anything else is native, and
     * must not be mistaken for one after subtracting a relocation delta. */
    if (target >= rt_image_lo && target < rt_image_hi)
        return rt_lookup(target - rt_reloc_delta);
    for (unsigned i = 0; i < g_nmodules; ++i)
        if (target >= g_modules[i].lo && target < g_modules[i].hi)
            return module_lookup(g_modules[i].m, target - *g_modules[i].m->delta);
    return NULL;
}

/* The binary searches above were 5.7% of the game thread on an Xbox One X (every indirect call and
 * virtual method comes through here), so the translations found are cached: per thread (no locking),
 * direct-mapped, and dropped whenever the image or a module is placed (g_lookup_gen). A target with
 * no translation (an import, a thunk) is not cached: it keeps taking the cheap range checks. */
#if defined(_MSC_VER)
#define LOOKUP_TLS __declspec(thread)
#elif defined(__SWITCH__)
/* one cache for all: translated code runs only under the guest lock, one thread at a time, and a thread-local's
 * every access is a call here (-mtp=soft: __aarch64_read_tp, 1.2% of the game thread with the shims' own) */
#define LOOKUP_TLS
#else
#define LOOKUP_TLS _Thread_local
#endif
#define LOOKUP_CACHE 4096u /* entries; a power of two */
typedef struct
{
    uint32_t target, gen;
    GuestFn fn;
} LookupEnt;
static LOOKUP_TLS LookupEnt t_lookup[LOOKUP_CACHE];

GuestFn rt_lookup_any(uint32_t target)
{
    LookupEnt* e = &t_lookup[(target ^ (target >> 12)) & (LOOKUP_CACHE - 1)];
    if (e->target == target && e->gen == g_lookup_gen && e->fn)
        return e->fn;
    GuestFn fn = lookup_uncached(target);
    if (fn)
        e->target = target, e->gen = g_lookup_gen, e->fn = fn;
    return fn;
}

#if defined(RT_WATERMARK)
void watermark_probe(Guest* g, uint32_t target); /* runtime/portable/watermark.c */
#endif

/* The guest's setjmp/longjmp. The CRT's longjmp restores the registers _setjmp3 saved and ends in
 * `jmp [buf+0x14]`, an indirect jump to the instruction after the call to _setjmp3: the middle of
 * a translated function, which no table holds (D3DX's JPEG and PNG readers bail out of a file they
 * cannot read this way, and the image loader tries the JPEG reader on every format it has not
 * matched yet). So the translation follows each call to _setjmp3 with a host setjmp (guest.h
 * RT_SETJMP), recorded here under that resume address and the guest esp there; the longjmp's jump
 * finds the record (same address, and the esp longjmp restored) and longjmps the host back into
 * that frame, which reloads the registers from g and carries on as the guest would. Translated
 * frames are plain C, so the host longjmp unwinds them like kobj.c's ExitThread does.
 *
 * Records live per thread and are never removed: a setjmp's frame may return at any time without
 * telling anyone. A longjmp into a frame that has returned is undefined in the guest as well; a
 * record is only ever reused for the same (address, esp) or, when all are taken, the oldest. */
#define SJ_SLOTS 16u
typedef struct
{
    uint32_t resume, esp, stamp;
    jmp_buf jb;
} SjSlot;
static LOOKUP_TLS SjSlot t_sj[SJ_SLOTS];
static LOOKUP_TLS uint32_t t_sj_clock;

jmp_buf* rt_setjmp_slot(Guest* g, uint32_t resume)
{
    SjSlot* s = NULL;
    for (unsigned i = 0; i < SJ_SLOTS && !s; ++i)
        if (t_sj[i].stamp && t_sj[i].resume == resume && t_sj[i].esp == g->esp)
            s = &t_sj[i];
    if (!s)
    {
        s = &t_sj[0];
        for (unsigned i = 1; i < SJ_SLOTS; ++i)
            if (t_sj[i].stamp < s->stamp)
                s = &t_sj[i];
    }
    s->resume = resume;
    s->esp = g->esp;
    s->stamp = ++t_sj_clock;
    return &s->jb;
}

void rt_longjmp_check(Guest* g, uint32_t target)
{
    if (!t_sj_clock)
        return;
    for (unsigned i = 0; i < SJ_SLOTS; ++i)
        if (t_sj[i].stamp && t_sj[i].resume == target && t_sj[i].esp == g->esp)
            longjmp(t_sj[i].jb, 1);
}

void rt_call_indirect(Guest* g, uint32_t target)
{
#if defined(RT_WATERMARK)
    watermark_probe(g, target);
#endif
#if defined(__SWITCH__)
    /* an import's or a COM method's thunk (thunk.h, THUNK_BASE): above any image or module, so never translated and
     * never in the cache below, which every D3D call missed */
    if (target >= 0xFFF00000u && g_native && g_native(g, target))
        return;
#endif
    GuestFn fn = rt_lookup_any(target);
    if (fn)
    {
        fn(g);
        return;
    }
    rt_longjmp_check(g, target);
    /* Not a translated function: an import (IAT slot holding a host API address) or a bug.
     * The platform layer decides; without one this is fatal. */
    if (g_native && g_native(g, target))
        return;
    rt_fatal(g, target, "indirect call/jump to an address with no translation");
}

/* Nonzero while a guest thread waits for the guest lock; the platform bridge maintains it and
 * installs the yield. Without a bridge (tests/runtime/difftest.c) there is one thread and no lock. */
volatile uint32_t rt_lock_contended;
static void (*g_yield)(void);

void rt_set_yield(void (*yield)(void)) { g_yield = yield; }

void rt_safepoint(void)
{
    if (g_yield)
        g_yield();
}

static FILE* g_log;
static RtFatalHook g_fatal_hook;

void rt_set_log(FILE* f) { g_log = f; }
void rt_set_fatal_hook(RtFatalHook h) { g_fatal_hook = h; }

/* rt_log from more than one thread (the d3d8 worker's): their lock, which the platform gives (rt_set_log_lock) */
static void (*g_log_lock)(void), (*g_log_unlock)(void);
void rt_set_log_lock(void (*lock)(void), void (*unlock)(void)) { g_log_lock = lock, g_log_unlock = unlock; }

void rt_log(const char* fmt, ...)
{
    va_list ap;
    void (*unlock)(void) = g_log_unlock;
    if (g_log_lock && unlock)
        g_log_lock();
    else
        unlock = NULL;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    if (g_log)
    {
        va_start(ap, fmt);
        vfprintf(g_log, fmt, ap);
        va_end(ap);
        fflush(g_log);
    }
    if (unlock)
        unlock();
}

/* A call in a function with its x87 stack in fixed locals (recomp/x87_slots.py) came back with the top where the
 * translation didn't expect it: it took the callee for one returning a float when it doesn't, or the other way. Logged
 * for the first 16 addresses; the function goes on (its x87 values may be wrong from here). */
void rt_x87_off(Guest* g, uint32_t at)
{
    static uint32_t said[16];
    static unsigned n;
    for (unsigned i = 0; i < n; ++i)
        if (said[i] == at)
            return;
    if (n < 16)
    {
        said[n++] = at;
        rt_log("[recomp] x87: the call at %08x left the stack's top at %u, not where its translation expects\n", at,
            (unsigned)g->top);
    }
}

/* A test build's check (RT_REGS_CHECK, guest.h): the call at `at` left a register the translation keeps in its local
 * (recomp/regs_across_calls.py says the callee gives it back) changed. Logged for the first 32 calls and registers. */
void rt_regs_off(uint32_t at, unsigned reg, uint32_t mine, uint32_t left)
{
    static const char* const names[8] = { "eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi" };
    static uint32_t said[32];
    static unsigned n;
    uint32_t key = at ^ (reg << 28);
    for (unsigned i = 0; i < n; ++i)
        if (said[i] == key)
            return;
    if (n < 32)
    {
        said[n++] = key;
        rt_log("[recomp] regs: the call at %08x changed %s (%08x, kept %08x), which its translation doesn't reload\n", at,
            names[reg & 7u], left, mine);
    }
}

void rt_fatal(Guest* g, uint32_t addr, const char* what)
{
    rt_log("\n[recomp] FATAL at %08x: %s\n", addr, what);
    rt_log("  eax=%08x ecx=%08x edx=%08x ebx=%08x esp=%08x ebp=%08x esi=%08x edi=%08x\n",
        g->eax, g->ecx, g->edx, g->ebx, g->esp, g->ebp, g->esi, g->edi);
    /* The guest stack, as return addresses give it: which guest functions led here. */
    rt_log("  guest stack:");
    for (int i = 0; i < 24; ++i)
        rt_log(" %08x", rd32(g->esp + 4u * i));
    rt_log("\n");
    if (g_fatal_hook)
        g_fatal_hook(addr, what);
    fflush(NULL); /* abort flushes nothing, and stderr may be buffered (host64.c) */
    abort();
}

/* The CPU the game is told it runs on: the host's CPUID with every SIMD extension removed
 * (MMX, SSE*, 3DNow!), so the game and D3DX take their x87 paths. The 203 functions that use
 * SIMD are not translated; if one is ever reached it traps (RT_UNIMPL), which is the test of
 * this choice. */
void rt_cpuid(Guest* g)
{
#if defined(_M_IX86)
    int r[4];
    uint32_t leaf = g->eax;
    __cpuidex(r, (int)leaf, (int)g->ecx);
    if (leaf == 1)
    {
        r[3] &= ~((1 << 23) | (1 << 25) | (1 << 26)); /* EDX: MMX, SSE, SSE2 */
        r[2] = 0;                                     /* ECX: SSE3 and later */
    }
    else if (leaf == 7)
    {
        r[1] = r[2] = r[3] = 0; /* AVX2, AVX-512 ... */
    }
    else if (leaf == 0x80000001u)
    {
        r[3] &= ~((1u << 31) | (1 << 30) | (1 << 23) | (1 << 22)); /* 3DNow!, 3DNow!+, MMX, MMX+ */
        r[2] = 0;
    }
    g->eax = (uint32_t)r[0];
    g->ebx = (uint32_t)r[1];
    g->ecx = (uint32_t)r[2];
    g->edx = (uint32_t)r[3];
#else
    /* Every other host (R3: x64 and arm64) reports one fixed CPU, the same everywhere: a
     * Pentium III-class GenuineIntel (family 6, model 8) with FPU, TSC, CX8 and CMOV, and no MMX,
     * SSE, FXSR or extended leaves. */
    uint32_t leaf = g->eax;
    g->eax = g->ebx = g->ecx = g->edx = 0;
    if (leaf == 0)
    {
        g->eax = 1;
        g->ebx = 0x756E6547u; /* "Genu" */
        g->edx = 0x49656E69u; /* "ineI" */
        g->ecx = 0x6C65746Eu; /* "ntel" */
    }
    else if (leaf == 1)
    {
        g->eax = 0x00000683u;
        g->edx = (1u << 0) | (1u << 4) | (1u << 8) | (1u << 15); /* FPU, TSC, CX8, CMOV */
    }
    else if (leaf == 0x80000000u)
        g->eax = 0x80000000u; /* no extended leaves */
#endif
}

void rt_rdtsc(Guest* g)
{
#if defined(_M_IX86)
    unsigned long long t = __rdtsc();
#else
    unsigned long long t = rt_monotonic_ns() * 2; /* a 2 GHz TSC */
#endif
    g->eax = (uint32_t)t;
    g->edx = (uint32_t)(t >> 32);
}
