#include "geometry_guest.h"
#include "geometry_simd.h"
#include "geometry_test_memory.h"
#include "guest.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

unsigned char* rt_guest_base;
uint32_t rt_reloc_delta, rt_image_lo, rt_image_hi;
volatile uint32_t rt_lock_contended;
/* stand-ins for the scheduler (gthread.c) and the guest window (gwin.c) */
static int noyield_depth;
static int owner = 1;
int gt_holds(void)
{
    return owner;
}
void gt_noyield(int on)
{
    noyield_depth += on ? 1 : -1;
    if (noyield_depth < 0)
        abort();
}
static uint32_t blocked_page = UINT32_MAX, probe_count, executed;
int gwin_is_committed(uint32_t a)
{
    ++probe_count;
    return a != blocked_page;
}
static void body(Guest* g)
{
    (void)g;
    if (noyield_depth < 1)
        abort();
    ++executed;
}
static const GeometryLayout layout = {0x10454538, 0x104595c4, 0x104553a0, 0x10459468};
static unsigned char before[0x24000], globals[0x20000];
static void need(int ok, unsigned mode, const char* why)
{
    if (!ok)
    {
        fprintf(stderr, "FAIL mode%u %s\n", mode, why);
        exit(1);
    }
}
static Guest setup(unsigned dual, unsigned remap)
{
    memset(GUEST_PTR(0), 0, 0x24000);
    memset(GUEST_PTR(0x10454000), 0, 0x20000);
    Guest g;
    memset(&g, 0, sizeof g);
    g.ecx = 0x1000;
    g.esp = 0x1e000;
    g.fcw = 0x023f;
    wr32(0x10454538, 0x2400);
    wr32(0x104553a0, 0x4000);
    wr16(0x10459468, 2);
    wr16(0x1045946a, 2);
    wr32(0x1042, 0x2800);
    wr16(0x2800, 2);
    wr32(0x1048, 0x3000);
    wr32(0x105e, 0x2000);
    wr8(0x1032, remap ? 0x80 : 0);
    wr8(0x1034, dual ? 1 : 0);
    wr32(0x103c, 0x7000);
    wr32(0x205c, 0x2900);
    wr32(0x2060, 2);
    wr32(0x206e, 0x8000);
    wr32(0x2076, 0xa000);
    wr32(0x208e, 0xc000);
    wr32(0x2092, 0xd000);
    wr32(0x2096, 0xe000);
    wr32(0x209a, 0xf000);
    for (unsigned i = 0; i < 128; ++i)
        wr16(0x7000 + i * 2, (uint16_t)i);
    wr16(0x3000, 1 | (2 << 7));
    wr16(0x3004, 3 | (4 << 7));
    wr16(0x3008, 5 | (6 << 7));
    wr16(0x300a, 7 | (8 << 7));
    wr16(0x300c, 9 | (10 << 7));
    wr16(0x300e, 11 | (12 << 7));
    wr16(0x2900, 0);
    wr16(0x2902, 3);
    blocked_page = UINT32_MAX;
    probe_count = 0;
    return g;
}
static void check(unsigned mode, Guest* g, int admitted)
{
    Guest state = *g;
    memcpy(before, GUEST_PTR(0), sizeof before);
    memcpy(globals, GUEST_PTR(0x10454000), sizeof globals);
    unsigned count = executed;
    int old_depth = noyield_depth;
    int result = geometry_guest_run(g, &layout, body);
    need(noyield_depth == old_depth, mode, "unbalanced ownership scope");
    need(result == admitted, mode, "admission");
    need(executed == count + (unsigned)admitted, mode, "entry calls");
    need(!memcmp(&state, g, sizeof state), mode, "Guest modified by guard");
    need(!memcmp(before, GUEST_PTR(0), sizeof before) && !memcmp(globals, GUEST_PTR(0x10454000), sizeof globals), mode,
         "memory modified by guard");
}
int main(void)
{
    rt_guest_base = geometry_test_reserve();
    need(rt_guest_base != NULL, 0, "reserve");
    geometry_test_commit(rt_guest_base, 0, 0x24000);
    geometry_test_commit(rt_guest_base, 0x10454000, 0x20000);
    geometry_test_commit(rt_guest_base, 0xffffc000, 0x4000);
    need(geometry_simd_supported(), 0, "host FP controls");
    unsigned cases = 0;
    for (unsigned dual = 0; dual < 2; ++dual)
        for (unsigned remap = 0; remap < 2; ++remap)
        {
            Guest g = setup(dual, remap);
            check(cases++, &g, 1);
        }
    for (unsigned mode = 0; mode < 27; ++mode)
    {
        Guest g = setup(1, 1);
        switch (mode)
        {
        case 0:
            wr32(0x208e, 0x8004);
            break;
        case 1:
            wr32(0x2092, 0x4040);
            break;
        case 2:
            wr32(0x2096, 0xc004);
            break;
        case 3:
            wr32(0x209a, 0xd004);
            break;
        case 4:
            wr32(0x208e, 0x3000);
            break;
        case 5:
            wr32(0x208e, 0x2900);
            break;
        case 6:
            wr32(0x208e, 0x7000);
            break;
        case 7:
            wr32(0x208e, 0x1034);
            break;
        case 8:
            wr32(0x208e, 0x205c);
            break;
        case 9:
            wr32(0x208e, 0x10459468);
            break;
        case 10:
            wr32(0x208e, g.esp - 64);
            break;
        case 11:
            wr32(0x206e, g.esp - 128);
            break;
        case 12:
            wr16(0x2800, 1);
            break;
        case 13:
            wr16(0x2902, 4);
            break;
        case 14:
            wr32(0x2060, 0x80000000);
            break;
        case 15:
            wr32(0x2060, 65535);
            break;
        case 16:
            wr32(0x208e, 0xfffffffc);
            break;
        case 17:
            wr32(0x206e, 0xfffffffc);
            break;
        case 18:
            wr32(0x104553a0, 0xffffffc0);
            break;
        case 19:
            wr32(0x103c, 0xffffffff);
            break;
        case 20:
            g.esp = 128;
            break;
        case 21:
            g.esp = 0xfffffffe;
            break;
        case 22:
            blocked_page = 0x8000;
            break;
        case 23:
            blocked_page = 0xc000;
            break;
        case 24:
            blocked_page = 0x4000;
            break;
        case 25:
            blocked_page = 0x7000;
            break;
        case 26:
            g.ecx = 0xfffffff0;
            break;
        }
        check(cases++, &g, 0);
    }
    /* Zero work must stop before following any unused array/remap pointer. */
    Guest g = setup(1, 1);
    wr32(0x10459468, 0);
    wr32(0x2060, 0);
    wr32(0x1042, 0xffffffff);
    wr32(0x1048, 0xffffffff);
    wr32(0x103c, 0xffffffff);
    check(cases++, &g, 0);
    need(probe_count < 16, cases, "zero-work array scan");
    /* Sparse remap: only used entries may be read, including a table that
     * straddles an unavailable page beyond its final actually-used entry. */
    g = setup(0, 1);
    wr16(0x10459468, 1);
    wr16(0x1045946a, 0);
    wr16(0x2800, 1);
    wr32(0x2060, 0);
    wr16(0x3000, 0);
    wr32(0x103c, 0x7ffe);
    wr16(0x7ffe, 1);
    blocked_page = 0x8000;
    wr32(0x206e, 0xa000);
    check(cases++, &g, 1);
    /* Non-dual rigid path must not read its unused second descriptor word. */
    g = setup(0, 0);
    wr16(0x10459468, 1);
    wr16(0x1045946a, 0);
    wr16(0x2800, 1);
    wr32(0x2060, 0);
    wr32(0x1048, 0x6ffe);
    wr16(0x6ffe, 1);
    blocked_page = 0x7000;
    check(cases++, &g, 1);
    /* Initial feature/callback admission must obey the same bounds/maps as
     * the alias preflight, before it dereferences those guest addresses. */
    for (unsigned mode = 0; mode < 6; ++mode)
    {
        g = setup(0, 0);
        switch (mode)
        {
        case 0:
            blocked_page = 0x10454000;
            break;
        case 1:
            blocked_page = 0x2000;
            break;
        case 2:
            blocked_page = 0x10459000;
            break;
        case 3:
            wr32(0x10454538, 0xfffffff7);
            break;
        case 4:
            wr32(0x10454538, 0xfffffff6);
            break;
        case 5:
            wr32(0x10454538, 0xfffffff3);
            blocked_page = 0xfffff000;
            break;
        }
        check(cases++, &g, 0);
    }
    g = setup(0, 0);
    wr32(0x10454538, 0xfffffff3);
    check(cases++, &g, 1);
    /* both feature bytes are checked as one span */
    g = setup(0, 0);
    wr32(0x10454538, 0x2ff6);
    wr8(0x2fff, 1);
    blocked_page = 0x3000;
    check(cases++, &g, 0);
    /* Work budget rejects before output validation or descriptor/remap scans. */
    for (unsigned mode = 0; mode < 4; ++mode)
    {
        g = setup(mode == 2, 0);
        unsigned r = mode == 0 ? 4097 : mode == 3 ? 1 : 0, w = mode == 1 ? 2049 : mode == 2 ? 1025 : 0;
        wr16(0x10459468, (uint16_t)r);
        wr16(0x1045946a, (uint16_t)w);
        wr16(0x2800, (uint16_t)r);
        wr32(0x2060, mode == 3 ? 2048 : 0);
        wr32(0x1048, 0xffffffff);
        wr32(0x206e, 0xffffffff);
        wr32(0x208e, 0xffffffff);
        check(cases++, &g, 0);
        need(probe_count < 20, cases, "work bound before array probes");
    }
    /* Ownership, control state, stack pages and metadata relocation fail closed. */
    g = setup(0, 0);
    owner = 0;
    check(cases++, &g, 0);
    owner = 1;
    g = setup(0, 0);
    g.fcw = 0x033f;
    check(cases++, &g, 0);
    g = setup(0, 0);
    g.fcw = 0x063f;
    check(cases++, &g, 0);
    g = setup(0, 0);
    wr8(0x2409, 1);
    check(cases++, &g, 0);
    g = setup(0, 0);
    wr8(0x240c, 1);
    check(cases++, &g, 0);
    g = setup(0, 0);
    wr32(layout.callback, 1);
    check(cases++, &g, 0);
    g = setup(0, 0);
    blocked_page = 0x1d000;
    check(cases++, &g, 0);
    g = setup(0, 0);
    rt_reloc_delta = 0xf0000000;
    check(cases++, &g, 0);
    rt_reloc_delta = 0;
    g = setup(0, 0);
    Guest* inside = (Guest*)GUEST_PTR(0x18000);
    *inside = g;
    check(cases++, inside, 0);
    need(!geometry_guest_run(NULL, &layout, body), cases++, "null Guest");
    need(!geometry_guest_run(&g, NULL, body), cases++, "null layout");
    need(!geometry_guest_run(&g, &layout, NULL), cases++, "null body");
    printf("{\"alias_guard_cases\":%u,\"unexpected_admissions\":0,\"Guest_or_memory_changes\":0}\n", cases);
    geometry_test_release(rt_guest_base);
    return 0;
}
