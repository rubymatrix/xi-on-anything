/* Imports on 64-bit hosts (R3): every Win32 function the game can reach - through its import
 * table or GetProcAddress - gets a synthetic guest address in a range that is never mapped. The
 * game calls it like any function pointer; rt_call_indirect finds no translation there and hands
 * it to thunk_dispatch, which runs the shim registered for that name. A shim knows its
 * signature: it reads its arguments off the guest stack and pops them itself (stdcall).
 *
 * An import with no shim still gets an address, and traps with its name on first call - the list
 * of shims to write grows from real runs, as it did for the R3.0 boundary trace. */
#pragma once

#include "runtime.h"

#define THUNK_BASE 0xFFF00000u
#define THUNK_STRIDE 16u
#define THUNK_MAX ((0xFFFF0000u - THUNK_BASE) / THUNK_STRIDE)

typedef void (*Shim)(Guest* g);

typedef struct ShimDef
{
    const char* dll; /* NULL: any DLL */
    const char* name; /* "#n" for an import by ordinal */
    Shim fn;
} ShimDef;

/* Run before every shim of a DLL (thunk_pre_rule), with the thunk's index: d3d8.c's waits for its worker thread
 * before a call that isn't one the worker runs. */
typedef void (*ThunkPre)(uint32_t thunk);
/* pre before every shim of dll ("d3d8.dll") but those exempt(name) says (the name after the '!'). */
void thunk_pre_rule(const char* dll, ThunkPre pre, int (*exempt)(const char* name));
/* "DLL!name" of a thunk by its index (a ThunkPre's argument). */
const char* thunk_name_at(uint32_t thunk);

/* Adds a NULL-terminated table of shims (k32.c and friends call this from their init). */
void thunk_register(const ShimDef* defs);
/* The guest address for dll!name, created on first request. */
uint32_t thunk_for(const char* dll, const char* name);
/* rt_call_indirect's native handler. */
int thunk_dispatch(Guest* g, uint32_t target);
/* "DLL!name" for a thunk address, or NULL. */
const char* thunk_name(uint32_t addr);
/* Profiling: called with the time of every outermost shim call (nested ones - a window procedure
 * calling back into shims - are inside it), on the calling thread. NULL: not timed. */
extern void (*thunk_timer)(uint64_t ns);
/* ... and per shim, for this thread (the game's: host64 sets it at Present); thunk_prof_report
 * logs the top shims by time since the last report. */
extern uint32_t thunk_prof_thread;
void thunk_prof_report(void);
/* The game thread's frame ended (gfx_queue.c): logs a long one with its top shims (FFXI_PROFILE), and starts the next. */
void thunk_frame_end(uint32_t frame, uint64_t frame_ns, uint64_t render_wait_ns, uint64_t lock_wait_ns);
/* Logs every bound import that has no shim yet; returns how many. */
unsigned thunk_report_missing(void);

/* Writing shims. Arguments are counted from 0 (the first parameter). */
#define ARG(n) rd32(g->esp + 4u + 4u * (n))
#define ARGP(n) ((void*)GUEST_PTR(ARG(n)))
#define ARGS(n) ((const char*)GUEST_PTR(ARG(n)))
/* stdcall return: pops the return address and nargs arguments */
#define RET(v, nargs) do { g->eax = (uint32_t)(v); g->esp += 4u + 4u * (nargs); return; } while (0)
/* cdecl return (varargs APIs such as wsprintfA): the caller pops the arguments */
#define RETC(v) do { g->eax = (uint32_t)(v); g->esp += 4u; return; } while (0)
