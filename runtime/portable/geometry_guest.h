/* The game's skinning routine on geometry_simd.h's kernels, through the recompiler's function
 * wrappers (geometry_hooks.c), for builds whose meta/builds.json "geometry" layout is verified.
 * Used by every game host. */
#pragma once
#include "guest.h"

typedef struct GeometryLayout
{
    /* guest addresses for one verified layout; geometry_guest_run relocates them */
    uint32_t info, callback, palette, counts;
} GeometryLayout;

enum
{
    GEOMETRY_MAX_WORK = 4096 /* the most weighted work units one admitted batch may run */
};

/* Runs body, the translated parent, if this batch passes every guard; returns 0 without touching
 * Guest or guest memory if it does not, and the caller then runs the original. The caller holds the
 * guest lock; it is kept (no yield) through the guards and the bounded work. body must be the
 * verified parent for this layout, which makes no callbacks. */
int geometry_guest_run(Guest* g, const GeometryLayout* layout, GuestFn body);
/* Inside an admitted parent, on the same thread and Guest: the CPU feature getters report SSE and
 * the two leaves run the kernels. Every other call runs original. */
void geometry_guest_feature(Guest* g, GuestFn original);
void geometry_guest_rigid(Guest* g, GuestFn original);
void geometry_guest_weighted(Guest* g, GuestFn original);
/* Installs the wrappers if FFXI_NATIVE_GEOMETRY=1 and this build has a verified layout. Called
 * after the game image has loaded. */
void geometry_hooks_init(void);
