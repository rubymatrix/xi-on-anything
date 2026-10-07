/* Production wrapper installation and scope tests; no generated game code. */
#include "build.h"
#include "geometry_guest.h"
#include "geometry_test_memory.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
unsigned char* rt_guest_base;
uint32_t rt_reloc_delta;
static int depth, calls, feature_calls, leaf_calls, exercised;
static const GeometryLayout layout = {0x10454538, 0x104595c4, 0x104553a0, 0x10459468};
static void need(int ok, const char* why)
{
    if (!ok)
    {
        fprintf(stderr, "FAIL %s\n", why);
        exit(1);
    }
}
int gt_holds(void)
{
    return 1;
}
void gt_noyield(int on)
{
    depth += on ? 1 : -1;
    need(depth >= 0, "balanced scope");
}
int gwin_is_committed(uint32_t a)
{
    return a < 0x20000 || (a >= 0x10454000 && a < 0x10474000);
}
void rt_log(const char* fmt, ...)
{
    (void)fmt;
}
static void feature(Guest* g)
{
    ++feature_calls;
    g->eax = 0;
    g->esp += 4;
}
static void leaf(Guest* g)
{
    (void)g;
    ++leaf_calls;
}
static void parent(Guest* g);
GuestFn rt_wrap_geometry_parent, rt_wrap_geometry_feature_sse, rt_wrap_geometry_feature_sse2;
GuestFn rt_wrap_geometry_rigid, rt_wrap_geometry_weighted;
const GuestFn rt_orig_geometry_parent = parent, rt_orig_geometry_feature_sse = feature,
              rt_orig_geometry_feature_sse2 = feature;
const GuestFn rt_orig_geometry_rigid = leaf, rt_orig_geometry_weighted = leaf;
static void parent(Guest* g)
{
    ++calls;
    if (!depth)
        return; /* original fallback */
    need(!geometry_guest_run(g, &layout, parent), "nested adapter rejected");
    Guest other = *g;
    geometry_guest_feature(&other, feature);
    need(other.eax == 0, "different Guest uses original");
    unsigned sp = g->esp;
    rt_wrap_geometry_feature_sse(g);
    need(g->eax == 0x2401 && g->esp == sp + 4, "scoped SSE feature");
    g->esp = sp;
    rt_wrap_geometry_feature_sse2(g);
    need(g->eax == 0x2401, "scoped SSE2 feature");
    g->esp = sp;
    wr32(sp + 4, 0x4000);
    wr32(sp + 8, 0x8000);
    wr32(sp + 12, 0xc000);
    wr32(sp + 16, 0xd000);
    for (unsigned i = 0; i < 16; ++i)
        wrf32(0x4000 + 4 * i, i % 5 == 0 ? 1.0f : 0.0f);
    for (unsigned i = 0; i < 6; ++i)
        wrf32(0x8000 + 4 * i, (float)(i + 1));
    rt_wrap_geometry_rigid(g);
    need(g->ecx == 0xc000 && g->edx == 0xd000 && g->esp == sp + 4, "rigid registers");
    need(!memcmp(GUEST_PTR(0xc000), GUEST_PTR(0x8000), 12), "rigid position");
    need(!memcmp(GUEST_PTR(0xd000), GUEST_PTR(0x800c), 12), "rigid normal");
    need(rd8(0x2409) == 0 && rd8(0x240c) == 0, "global features unchanged");
    ++exercised;
}
static void enable(const char* s)
{
#ifdef _WIN32
    need(_putenv_s("FFXI_NATIVE_GEOMETRY", s) == 0, "set environment");
#else
    need(setenv("FFXI_NATIVE_GEOMETRY", s, 1) == 0, "set environment");
#endif
}
int main(void)
{
    enable("0");
    geometry_hooks_init();
    need(!rt_wrap_geometry_parent, "default disabled");
    enable("yes");
    geometry_hooks_init();
    need(!rt_wrap_geometry_parent, "explicit one required");
    enable("1");
#if FFXI_GEOMETRY_LAYOUT == 0
    geometry_hooks_init();
    need(!rt_wrap_geometry_parent && !calls && !feature_calls && !leaf_calls && !exercised,
         "unsupported build unchanged");
#else
    rt_wrap_geometry_rigid = leaf;
    geometry_hooks_init();
    need(!rt_wrap_geometry_parent && rt_wrap_geometry_rigid == leaf, "occupied hooks retained");
    rt_wrap_geometry_rigid = NULL;
    geometry_hooks_init();
    need(rt_wrap_geometry_parent && rt_wrap_geometry_feature_sse && rt_wrap_geometry_feature_sse2 &&
             rt_wrap_geometry_rigid && rt_wrap_geometry_weighted,
         "complete hook set");
    Guest g;
    memset(&g, 0, sizeof g);
    g.esp = 0x10000;
    rt_wrap_geometry_feature_sse(&g);
    rt_wrap_geometry_rigid(&g);
    rt_wrap_geometry_weighted(&g);
    need(feature_calls == 1 && leaf_calls == 2, "out-of-scope originals");
    rt_guest_base = geometry_test_reserve();
    need(rt_guest_base != NULL, "reserve");
    geometry_test_commit(rt_guest_base, 0, 0x20000);
    geometry_test_commit(rt_guest_base, 0x10454000, 0x20000);
    g.fcw = 0x033f;
    rt_wrap_geometry_parent(&g);
    need(calls == 1 && exercised == 0, "guard fallback original");
    g.fcw = 0x023f;
    g.ecx = 0x1000;
    g.esp = 0x1e000;
    wr32(layout.info, 0x2400);
    wr32(layout.counts, 1);
    wr32(layout.palette, 0x4000);
    wr32(0x1042, 0x2800);
    wr16(0x2800, 1);
    wr32(0x1048, 0x3000);
    wr32(0x105e, 0x2000);
    wr32(0x206e, 0x8000);
    wr32(0x208e, 0xc000);
    wr32(0x2092, 0xd000);
    rt_wrap_geometry_parent(&g);
    need(calls == 2 && exercised == 1 && depth == 0, "admitted parent scope");
    rt_wrap_geometry_feature_sse(&g);
    need(feature_calls == 3, "scope cleared after parent");
    geometry_test_release(rt_guest_base);
#endif
    printf("{\"hook_schema\":%d,\"failures\":0}\n", FFXI_GEOMETRY_LAYOUT);
    return 0;
}
