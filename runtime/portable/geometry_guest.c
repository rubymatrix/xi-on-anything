#include "geometry_guest.h"
#include "geometry_simd.h"
#include "gthread.h"
#include "gwin.h"
#include "plat.h"

#if !defined(RT_GUEST_WINDOW)
/* the browser (flat guest memory, no window): the kernels need a Guest outside guest memory and the host
 * SIMD of geometry_simd.c, so they stay off there and the game runs its own code */
#define rt_guest_base ((uint8_t*)0)
#endif

/* the batch this thread is running, if any; the feature bytes in guest memory are never changed */
static RT_TLS Guest* batch_guest;
static RT_TLS GeometryLayout batch_layout;

/* Admission for the SSE route. The original SSE code allows aliases, but a batch whose outputs
 * overwrite its own sources, matrices or metadata would carry the deliberate x87 (53-bit) versus SSE
 * rounding difference into later inputs, so such batches are rejected, before any write to Guest or
 * guest memory. What is checked stays true only while no other guest thread runs: the caller keeps
 * gt_noyield across the checks and the batch. */
typedef struct Span
{
    uint64_t begin, end;
} Span;
/* what a batch writes: its outputs and the stack below esp */
typedef struct Written
{
    Span output[4], stack;
    unsigned count;
} Written;

static int span(uint64_t start, uint64_t bytes, Span* range)
{
    if (!bytes || start > UINT32_MAX || bytes > 0x100000000ull - start)
        return 0;
    range->begin = start;
    range->end = start + bytes;
    return 1;
}
static int overlap(Span a, Span b)
{
    return a.begin < b.end && b.begin < a.end;
}
static int mapped(Span r)
{
    for (uint64_t p = r.begin & ~4095ull; p < r.end; p += 4096)
        if (!gwin_is_committed((uint32_t)p))
            return 0;
    return 1;
}
static int read16(uint64_t address, uint16_t* value)
{
    Span r;
    if (!span(address, 2, &r) || !mapped(r))
        return 0;
    *value = rd16((uint32_t)address);
    return 1;
}
static int read32(uint64_t address, uint32_t* value)
{
    Span r;
    if (!span(address, 4, &r) || !mapped(r))
        return 0;
    *value = rd32((uint32_t)address);
    return 1;
}
/* an input the parent reads: it must not overlap the stack or an output, and with check_pages its
 * pages must be mapped */
static int clear(const Written* a, uint64_t address, uint64_t bytes, int check_pages)
{
    Span r;
    if (!span(address, bytes, &r) || overlap(r, a->stack))
        return 0;
    for (unsigned j = 0; j < a->count; ++j)
        if (overlap(r, a->output[j]))
            return 0;
    return !check_pages || mapped(r);
}
/* whether the batch is in bounds, has work, aliases nothing it writes and reads only mapped pages */
static int geometry_alias_guard(const Guest* g, const GeometryLayout* layout)
{
    uint32_t header, indices, object, counts, raw_duplicates;
    uint16_t primary;
    uint64_t mesh = g->ecx;
    if (!read32(mesh + 0x42, &header) || !read32(mesh + 0x48, &indices) ||
        !read32(mesh + 0x5e, &object) || !read32(layout->counts, &counts) ||
        !read32((uint64_t)object + 0x60, &raw_duplicates))
        return 0;
    uint32_t rigid = counts & 65535u, weighted = counts >> 16;
    /* no array scans or reads through unused pointers when there is no work */
    if ((int32_t)raw_duplicates < 0 || (!rigid && !weighted))
        return 0;
    uint32_t duplicates = raw_duplicates;
    if (!read16(header, &primary) || primary != rigid)
        return 0;
    uint64_t count = (uint64_t)rigid + weighted + duplicates;
    if (count > 65535u || g->esp < 256u || g->esp > UINT32_MAX - 4u)
        return 0;
    uint16_t model_flags;
    Span mode_range;
    if (!read16(mesh + 0x32, &model_flags) || !span(mesh + 0x34, 1, &mode_range) || !mapped(mode_range))
        return 0;
    int dual = rd8((uint32_t)(mesh + 0x34)) & 1;
    /* bounds the scans below and the batch itself (in work units, not time) before any output or
     * array pointer is followed */
    uint64_t work = ((uint64_t)rigid + 2ull * weighted + 2ull * duplicates) * (dual ? 2u : 1u);
    if (work > GEOMETRY_MAX_WORK)
        return 0;
    Written a;
    a.count = dual ? 4 : 2;
    if (!span((uint64_t)g->esp - 256u, 260, &a.stack) || !mapped(a.stack))
        return 0;
    for (unsigned j = 0; j < a.count; ++j)
    {
        uint32_t output;
        if (!read32((uint64_t)object + 0x8eu + j * 4, &output) || !span(output, count * 12, &a.output[j]) ||
            overlap(a.output[j], a.stack))
            return 0;
        for (unsigned k = 0; k < j; ++k)
            if (overlap(a.output[j], a.output[k]))
                return 0;
        if (!mapped(a.output[j]))
            return 0;
    }
    /* metadata the parent reads must not alias an output; unused fields are not read */
    uint32_t info;
    if (!clear(&a, mesh + 0x32, 0x30, 0) || !clear(&a, (uint64_t)object + 0x5c, 0x42, 0) ||
        !clear(&a, header, 2, 0) || !clear(&a, layout->info, 4, 0) || !clear(&a, layout->callback, 4, 0) ||
        !clear(&a, layout->palette, 4, 0) || !clear(&a, layout->counts, 4, 0) || !read32(layout->info, &info) ||
        !clear(&a, (uint64_t)info + 9, 4, 0) || !clear(&a, indices, ((uint64_t)rigid + weighted) * 4, 0))
        return 0;
    uint64_t source_bytes = (uint64_t)rigid * 24 + (uint64_t)weighted * 56;
    uint32_t source;
    if (!read32((uint64_t)object + 0x6e, &source) || !clear(&a, source, source_bytes, 1))
        return 0;
    if (dual && (!read32((uint64_t)object + 0x76, &source) || !clear(&a, source, source_bytes, 1)))
        return 0;
    /* The descriptor words actually used, mapped as one span: a rigid descriptor's second word is
     * unused, so a final rigid descriptor's need not be mapped. Collects the bones (at most 128)
     * they name, so only the remap entries in use are read below. */
    uint64_t descriptor_bytes = weighted ? ((uint64_t)rigid + weighted) * 4 : (uint64_t)rigid * 4 - 2;
    Span descriptors;
    if (!span(indices, descriptor_bytes, &descriptors) || !mapped(descriptors))
        return 0;
    uint32_t used[4] = {0, 0, 0, 0};
    for (uint32_t j = 0; j < rigid + weighted; ++j)
    {
        unsigned words = j < rigid ? 1 : 2;
        for (unsigned k = 0; k < words; ++k)
        {
            uint16_t word = rd16((uint32_t)((uint64_t)indices + (uint64_t)j * 4 + k * 2));
            unsigned bone = word & 127u;
            used[bone >> 5] |= 1u << (bone & 31);
            if (dual)
            {
                bone = (word >> 7) & 127u;
                used[bone >> 5] |= 1u << (bone & 31);
            }
        }
    }
    if (duplicates)
    {
        uint32_t list;
        if (!read32((uint64_t)object + 0x5c, &list) || !clear(&a, list, (uint64_t)duplicates * 2, 1))
            return 0;
        for (uint32_t j = 0; j < duplicates; ++j)
            if (rd16((uint32_t)((uint64_t)list + (uint64_t)j * 2)) >= rigid + weighted)
                return 0;
    }
    uint32_t matrix_base, remap = 0;
    if (!read32(layout->palette, &matrix_base) || ((model_flags & 0x80u) && !read32(mesh + 0x3c, &remap)))
        return 0;
    uint64_t matrix_first = UINT64_MAX, matrix_end = 0;
    for (unsigned bone = 0; bone < 128; ++bone)
        if (used[bone >> 5] & (1u << (bone & 31)))
        {
            uint16_t slot = (uint16_t)bone;
            if (model_flags & 0x80u)
            {
                uint64_t address = (uint64_t)remap + bone * 2u;
                if (!clear(&a, address, 2, 0) || !read16(address, &slot))
                    return 0;
            }
            Span matrix;
            if (!span((uint64_t)matrix_base + (uint64_t)slot * 64, 64, &matrix) || !mapped(matrix))
                return 0;
            if (matrix.begin < matrix_first)
                matrix_first = matrix.begin;
            if (matrix.end > matrix_end)
                matrix_end = matrix.end;
        }
    /* one span from the first to the last matrix used: may reject an alias in an unused gap
     * between them, never reads that gap */
    return clear(&a, matrix_first, matrix_end - matrix_first, 0);
}

/* the layout's addresses with the module's relocation applied; 0 if one is unset or wraps */
static int relocated(const GeometryLayout* in, GeometryLayout* out)
{
    const uint32_t values[] = {in->info, in->callback, in->palette, in->counts};
    uint32_t result[4];
    for (unsigned i = 0; i < 4; ++i)
    {
        if (!values[i] || (uint64_t)values[i] + RD > UINT32_MAX)
            return 0;
        result[i] = values[i] + RD;
    }
    out->info = result[0];
    out->callback = result[1];
    out->palette = result[2];
    out->counts = result[3];
    return 1;
}

/* x87 control word 0x023f (53-bit, nearest, exceptions masked), supported host FP controls, a
 * Guest outside guest memory, no callback installed and both SSE feature bytes clear; then the
 * alias guard. */
static int admitted(Guest* g, const GeometryLayout* layout)
{
    if (g->fcw != 0x023f || !geometry_simd_supported())
        return 0;
    uintptr_t host = (uintptr_t)g, base = (uintptr_t)rt_guest_base;
    if (host - base <= UINT32_MAX || host + sizeof(*g) - 1 - base <= UINT32_MAX)
        return 0;
    uint32_t info, callback;
    if (!read32(layout->info, &info) || !info || !read32(layout->callback, &callback) || callback)
        return 0;
    Span features;
    if (!span((uint64_t)info + 9, 4, &features) || !mapped(features))
        return 0;
    if (rd8(info + 9) || rd8(info + 12))
        return 0;
    return geometry_alias_guard(g, layout);
}

int geometry_guest_run(Guest* g, const GeometryLayout* layout, GuestFn body)
{
    if (!g || !layout || !body || !rt_guest_base || batch_guest || !gt_holds())
        return 0;
    GeometryLayout current;
    if (!relocated(layout, &current))
        return 0;
    gt_noyield(1);
    int accepted = admitted(g, &current);
    if (accepted)
    {
        batch_layout = current;
        batch_guest = g;
        body(g);
        batch_guest = NULL;
    }
    gt_noyield(0);
    return accepted;
}

void geometry_guest_feature(Guest* g, GuestFn original)
{
    if (g != batch_guest || !g)
    {
        original(g);
        return;
    }
    g->eax = (rd32(batch_layout.info) & 0xffffff00u) | 1u;
    g->esp += 4;
}

/* The leaves are cdecl, (matrix, source, position, normal) and (matrix_a, matrix_b, source, position,
 * normal), and leave position in ecx and normal in edx. */
void geometry_guest_rigid(Guest* g, GuestFn original)
{
    if (g != batch_guest || !g)
    {
        original(g);
        return;
    }
    uint32_t sp = g->esp, matrix = rd32(sp + 4), source = rd32(sp + 8);
    uint32_t p = rd32(sp + 12), n = rd32(sp + 16);
    geometry_simd_rigid(GUEST_PTR(matrix), GUEST_PTR(source), GUEST_PTR(p), GUEST_PTR(n));
    g->ecx = p;
    g->edx = n;
    g->esp = sp + 4;
}

void geometry_guest_weighted(Guest* g, GuestFn original)
{
    if (g != batch_guest || !g)
    {
        original(g);
        return;
    }
    uint32_t sp = g->esp, a = rd32(sp + 4), b = rd32(sp + 8), source = rd32(sp + 12);
    uint32_t p = rd32(sp + 16), n = rd32(sp + 20);
    geometry_simd_weighted(GUEST_PTR(a), GUEST_PTR(b), GUEST_PTR(source), GUEST_PTR(p), GUEST_PTR(n));
    g->ecx = p;
    g->edx = n;
    g->esp = sp + 4;
}
