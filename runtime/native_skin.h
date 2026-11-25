/* FFXiMain's two-bone skinning loop in C (recomp/native_loops.py finds it by its bytes, and the translation calls this
 * just before the loop; 0x1002f277-0x1002f79b in the 2025-12-26 build, inside f_1002efe0).
 *
 * For each vertex i of a model (ecx: 14 floats, 0x38 bytes, pointing at its sixth), two bones' matrices out of the
 * game's matrix table (by an index pair, remapped through the model's table when its flag 0x80 says so) give its
 * position (the 4-vectors at floats 0, 2, 4, 6 and 1, 3, 5, 7, through the first and second matrix) and its normal
 * (8, 10, 12 and 9, 11, 13 through their 3x3 parts), each written to its own array; with the model's flag 1 the
 * same is done to a second vertex list (a 0x38-byte stride too, beside the first) through a second pair of bones.
 *
 * The x87 code does each product in double (float times float: exact) and adds in an order this keeps, rounding
 * to float only at the stores, so the results are the x87's. It runs only at the x87's 53- or 64-bit precision
 * (at 24 the translation runs: every sum would be rounded). The registers the code after the loop sees, and the
 * temporaries it left on the guest's stack, are the loop's last; the flags are those of its last `cmp eax, edx`
 * (equal), which nothing reads.
 *
 * FFXI_SKIN_CHECK=1: the results go to a buffer instead, the translated loop runs, and rt_skin_two_bone_check
 * (at the loop's exit) compares them with what it wrote, logging the first differences. */
#ifndef RT_NATIVE_SKIN_H
#define RT_NATIVE_SKIN_H

#include <stdlib.h>

typedef struct RtSkinOut
{
    int ok;
    uint32_t eax, ecx, edx, ebx, ebp, esi;
} RtSkinOut;

extern int rt_native_check; /* runtime.c: FFXI_SKIN_CHECK, -1 until read */
void rt_log(const char* fmt, ...);

/* check mode: what the native loop computed, to compare at the loop's exit */
typedef struct RtSkinPending
{
    int on;
    uint32_t out[4], n; /* the arrays (out3, out4 only with the second list) and the vertices */
    int second;
    float* v;           /* n * 12 floats: out1, out2, out3, out4 (3 each) per vertex */
    size_t cap;
} RtSkinPending;
/* (a function's static: a header's static variable would be one more unused one in every file including it) */
RT_INLINE RtSkinPending* rt_skin_pend(void)
{
    static RtSkinPending p;
    return &p;
}

RT_INLINE RtSkinOut rt_skin_two_bone(unsigned char* gb, uint32_t eax, uint32_t ecx, uint32_t edx, uint32_t ebx, uint32_t esp,
    uint32_t ebp, uint32_t esi, unsigned cw, uint32_t a_mtx, uint32_t a_idx, uint32_t a_cnt)
{
    RtSkinOut o = { 0 };
    (void)eax, (void)edx, (void)ebx;
    if (((cw >> 8) & 3u) == 0)
        return o;
    if (rt_native_check < 0)
    {
        const char* e = getenv("FFXI_SKIN_CHECK");
        rt_native_check = e && *e && *e != '0';
    }
    const int check = rt_native_check;
    uint32_t i = rd32_b(gb, esp + 0x34u);
    const uint32_t n = rd16_b(gb, a_cnt);
    uint32_t out1 = rd32_b(gb, esp + 0x18u), out2 = rd32_b(gb, esp + 0x1Cu);
    uint32_t out3 = rd32_b(gb, esp + 0x28u), out4 = rd32_b(gb, esp + 0x20u);
    const uint32_t obj = rd32_b(gb, esp + 0x14u), pairs = rd32_b(gb, esp + 0x30u);
    float* cv = NULL;
    if (check)
    {
        if ((int32_t)i >= (int32_t)n)
            return o;
        size_t need = (size_t)(n - i) * 12u;
        if (rt_skin_pend()->cap < need)
        {
            float* p = (float*)realloc(rt_skin_pend()->v, need * sizeof(float));
            if (!p)
                return o;
            rt_skin_pend()->v = p, rt_skin_pend()->cap = need;
        }
        cv = rt_skin_pend()->v;
        rt_skin_pend()->out[0] = out1, rt_skin_pend()->out[1] = out2;
        rt_skin_pend()->out[2] = out3, rt_skin_pend()->out[3] = out4;
        rt_skin_pend()->n = n - i, rt_skin_pend()->second = 0;
    }
    uint32_t mt = 0, b1 = 0, b2 = 0;
    float t4c = 0, t50 = 0, t54 = 0;
    for (;;)
    {
        /* the bones: [esp+0x24] the first's offset, eax the second's (in floats * 4: a matrix is 16 floats) */
        uint16_t w = (uint16_t)(rd16_b(gb, obj + 0x32u) & 0x80u);
        uint32_t e0 = rd16_b(gb, pairs + i * 4u), e1 = rd16_b(gb, pairs + i * 4u + 2u);
        if (!check)
            wr16_b(gb, esp + 0x12u, w), wr32_b(gb, esp + 0x38u, e0);
        if (w)
        {
            uint32_t remap = rd32_b(gb, obj + 0x3Cu);
            b1 = (uint32_t)rd16_b(gb, remap + (e0 & 0x7Fu) * 2u) << 4;
            b2 = rd16_b(gb, remap + (e1 & 0x7Fu) * 2u);
            if (!check)
                wr32_b(gb, esp + 0x48u, e1);
        }
        else
            b1 = (e0 & 0x7Fu) << 4, b2 = e1 & 0x7Fu;
        if (!check)
            wr32_b(gb, esp + 0x24u, b1);
        edx = e1;
        b2 <<= 4;
        int second = rd8_b(gb, obj + 0x34u) & 1u;
        if (second)
        {
            if (w)
            {
                uint32_t remap = rd32_b(gb, obj + 0x3Cu);
                esi = (uint32_t)rd16_b(gb, remap + ((e0 >> 7) & 0x7Fu) * 2u) << 4;
                ebp = (uint32_t)rd16_b(gb, remap + ((e1 >> 7) & 0x7Fu) * 2u) << 4;
                edx = (e1 >> 7) & 0x7Fu;
            }
            else
                esi = (e0 >> 3) & 0x7F0u, edx = (e1 >> 3) & 0x7F0u, ebp = edx;
        }
        mt = rd32_b(gb, a_mtx);
        /* the first list: position to out1, normal to out2 */
        double v[14];
        for (int k = 0; k < 14; ++k)
            v[k] = rdf32_b(gb, ecx - 0x14u + 4u * (uint32_t)k);
        for (uint32_t k = 0; k < 3; ++k)
        {
            uint32_t pa = mt + b1 * 4u + 4u * k, pb = mt + b2 * 4u + 4u * k;
            double a0 = rdf32_b(gb, pa), a1 = rdf32_b(gb, pa + 0x10u), a2 = rdf32_b(gb, pa + 0x20u),
                   a3 = rdf32_b(gb, pa + 0x30u);
            float fb0 = rdf32_b(gb, pb), fb1 = rdf32_b(gb, pb + 0x10u), fb2 = rdf32_b(gb, pb + 0x20u);
            double b0 = fb0, b1d = fb1, b2d = fb2, b3 = rdf32_b(gb, pb + 0x30u);
            double s = ((b3 * v[7] + b1d * v[3]) + b0 * v[1]) + b2d * v[5];
            double r = ((a3 * v[6] + a2 * v[4]) + a1 * v[2]) + a0 * v[0];
            double u = (b2d * v[13] + b1d * v[11]) + b0 * v[9];
            double q = (a2 * v[12] + a1 * v[10]) + a0 * v[8];
            float p = (float)(s + r), nrm = (float)(u + q);
            if (cv)
                cv[k] = p, cv[3 + k] = nrm;
            else
                wrf32_b(gb, out1 + 4u * k, p), wrf32_b(gb, out2 + 4u * k, nrm);
            t4c = fb0, t50 = fb1, t54 = fb2;
        }
        out1 += 0xCu, out2 += 0xCu;
        if (second)
        {
            /* the second list: through esi's and ebp's bones, to out3 and out4 */
            uint32_t list = rd32_b(gb, rd32_b(gb, esp + 0x2Cu) + 0x76u);
            uint32_t pv = list + (rd32_b(gb, a_idx) & 0xFFFFu) * 24u + rd32_b(gb, esp + 0x3Cu) + ecx;
            double p[14];
            for (int k = 0; k < 14; ++k)
                p[k] = rdf32_b(gb, pv + 4u * (uint32_t)k);
            float t5c = 0, t60 = 0, t64 = 0;
            for (uint32_t k = 0; k < 3; ++k)
            {
                uint32_t pc = mt + esi * 4u + 4u * k, pd = mt + ebp * 4u + 4u * k;
                double c0 = rdf32_b(gb, pc), c1 = rdf32_b(gb, pc + 0x10u), c2 = rdf32_b(gb, pc + 0x20u),
                       c3 = rdf32_b(gb, pc + 0x30u);
                float fd0 = rdf32_b(gb, pd), fd1 = rdf32_b(gb, pd + 0x10u), fd2 = rdf32_b(gb, pd + 0x20u);
                double d0 = fd0, d1 = fd1, d2 = fd2, d3 = rdf32_b(gb, pd + 0x30u);
                double x = ((c0 * p[0] + c1 * p[2]) + c3 * p[6]) + c2 * p[4];
                double y = ((d0 * p[1] + d1 * p[3]) + d3 * p[7]) + d2 * p[5];
                double z = (d0 * p[9] + d2 * p[13]) + d1 * p[11];
                double ww = (c0 * p[8] + c2 * p[12]) + c1 * p[10];
                float pos = (float)(x + y), nrm = (float)(z + ww);
                if (cv)
                    cv[6 + k] = pos, cv[9 + k] = nrm;
                else
                    wrf32_b(gb, out3 + 4u * k, pos), wrf32_b(gb, out4 + 4u * k, nrm);
                t5c = fd0, t60 = fd1, t64 = fd2;
            }
            out3 += 0xCu, out4 += 0xCu;
            ebx = out3, eax = out4, edx = mt;
            if (!check)
                wrf32_b(gb, esp + 0x5Cu, t5c), wrf32_b(gb, esp + 0x60u, t60), wrf32_b(gb, esp + 0x64u, t64);
            if (cv)
                rt_skin_pend()->second = 1;
        }
        else
            ebx = out3;
        if (cv)
            cv += 12;
        ++i;
        ecx += 0x38u;
        if ((int32_t)i >= (int32_t)n)
            break;
    }
    if (check)
    {
        rt_skin_pend()->on = 1;
        return o; /* the translated loop runs, and writes */
    }
    wrf32_b(gb, esp + 0x4Cu, t4c), wrf32_b(gb, esp + 0x50u, t50), wrf32_b(gb, esp + 0x54u, t54);
    wr32_b(gb, esp + 0x18u, out1), wr32_b(gb, esp + 0x1Cu, out2);
    wr32_b(gb, esp + 0x28u, out3), wr32_b(gb, esp + 0x20u, out4);
    wr32_b(gb, esp + 0x34u, i);
    o.ok = 1;
    o.eax = i, o.edx = n, o.ecx = ecx, o.ebx = ebx, o.ebp = ebp, o.esi = esi;
    return o;
}

/* check mode, at the loop's exit: the translated loop's results against the native one's */
RT_INLINE void rt_skin_two_bone_check(unsigned char* gb)
{
    if (!rt_skin_pend()->on)
        return;
    rt_skin_pend()->on = 0;
    static unsigned calls, bad_calls, said;
    static uint64_t values, bad_values;
    const float* cv = rt_skin_pend()->v;
    unsigned bad = 0;
    for (uint32_t i = 0; i < rt_skin_pend()->n; ++i, cv += 12)
        for (int s = 0; s < (rt_skin_pend()->second ? 4 : 2); ++s)
            for (int k = 0; k < 3; ++k)
            {
                float t = rdf32_b(gb, rt_skin_pend()->out[s] + 12u * i + 4u * (uint32_t)k), nv = cv[3 * s + k];
                values++;
                if (memcmp(&t, &nv, 4))
                {
                    bad_values++, bad++;
                    if (said < 8)
                        said++, rt_log("[recomp] skin check: vertex %u array %d [%d]: translated %.9g, native %.9g\n", i, s,
                                    k, (double)t, (double)nv);
                }
            }
    calls++;
    bad_calls += bad != 0;
    if ((calls & (calls - 1)) == 0 || (calls % 4096) == 0)
        rt_log("[recomp] skin check: %u loops, %u with a difference; %llu values, %llu different\n", calls, bad_calls,
            (unsigned long long)values, (unsigned long long)bad_values);
}

/* FFXiMain's point and direction transforms (0x10028040 and 0x100280d0 in 2025-12-26: m's 3x3 part times the vector,
 * plus m's fourth row for a point), called at 53 places, the skinning's second loop among them: translated inline at
 * each call (recomp/x86c.py, by their bytes) instead of a call that stores and reloads every register. The x87's
 * order, its rounding at the control word's precision (fr_cw), and its reads: each component's inputs are read just
 * before its store, so an output over the vector gives what the x87 code gives. */
RT_INLINE void rt_xform3(unsigned char* gb, unsigned cw, uint32_t m, uint32_t v, uint32_t out, int point)
{
    for (uint32_t k = 0; k < 3; ++k)
    {
        double a = fr_cw(cw, (double)rdf32_b(gb, m + 0x20u + 4u * k) * rdf32_b(gb, v + 8u));
        a = fr_cw(cw, a + fr_cw(cw, (double)rdf32_b(gb, m + 0x10u + 4u * k) * rdf32_b(gb, v + 4u)));
        a = fr_cw(cw, a + fr_cw(cw, (double)rdf32_b(gb, v) * rdf32_b(gb, m + 4u * k)));
        if (point)
            a = fr_cw(cw, a + rdf32_b(gb, m + 0x30u + 4u * k));
        wrf32_b(gb, out + 4u * k, (float)a);
    }
}

/* FFXiMain's 4x4 matrix products (recomp/x86c.py MAT_FNS), called inline: out = l * r (rows of l times r), written
 * once both were read, so out may be either. Each kind keeps the order its x87 code adds an element's four products
 * in, its rounding and its stores to float. At the x87's 53-bit precision a product of two floats is exact in a
 * double, so a fused multiply-add rounds as the multiply and the add do: the NEON version gives the same bits. */
enum { RT_MAT_THIS_ARG, RT_MAT_ARG_THIS, RT_MAT_STDCALL };
void rt_mat_mul(unsigned char* gb, unsigned cw, uint32_t out, uint32_t l, uint32_t r, int kind); /* runtime.c */
void rt_mat_mul_c(unsigned char* gb, unsigned cw, uint32_t out, uint32_t l, uint32_t r, int kind); /* (without NEON) */
RT_INLINE void rt_mat_copy(unsigned char* gb, uint32_t out, uint32_t in)
{
    for (uint32_t k = 0; k < 16; ++k) /* (in order: the two may overlap) */
        wr32_b(gb, out + 4u * k, rd32_b(gb, in + 4u * k));
}

#endif
