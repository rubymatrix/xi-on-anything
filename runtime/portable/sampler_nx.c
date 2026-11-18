/* A sampling profiler for the game thread on the Nintendo Switch: FFXI_SAMPLE=<file> (run.args), as
 * sampler_win.c on Windows, with the same file for tools/sample_report.py.
 *
 * Every millisecond a thread of its own pauses the game thread (svcSetThreadActivity), reads its registers
 * (svcGetThreadContext3) and lets it run again. A sample is its pc, its lr (the caller, for a function that
 * hasn't saved it yet) and the return addresses of the frame records x29 chains through the stack (as far as
 * functions keep frame pointers), relative to the .nro's base (_start, at its offset 0; __start__ is an absolute 0, not moved with the image); one outside the image (libnx's
 * syscalls, the kernel) is 0xFFFFFFFF. tools/sample_report.py resolves them against `aarch64-none-elf-nm` of the
 * .elf (--base 0).
 *
 * The file is written through the file system's own calls (fsFile*), not stdio: on the Switch, stdio from a
 * thread other than the main one broke the main thread's (docs/switch.md). Samples gather in a buffer that is
 * written each second.
 *
 * File: "FXSAMPL1", then records { u32 count; u32 rva[count]; }. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <switch.h>

#include "sampler.h"

#define SAMPLE_DEPTH 24
#define OUTSIDE 0xFFFFFFFFu
#define BUF_WORDS (256 * 1024) /* 1 MB: a second of samples is ~100 KB */

extern void _start(void); /* libnx's entry, the .nro's first byte: its runtime address is the image's base */
extern char __end__[];

static Handle g_game;
static Thread g_thread;
static FsFile g_file;
static s64 g_off;
static uint32_t g_buf[BUF_WORDS];
static size_t g_n;
static uint64_t g_stack_lo, g_stack_hi; /* the game thread's stack: where frame records may be */

static uint32_t rva(uint64_t a)
{
    uint64_t base = (uint64_t)(uintptr_t)&_start, end = (uint64_t)(uintptr_t)__end__;
    return a >= base && a < end ? (uint32_t)(a - base) : OUTSIDE;
}

static void flush_buf(void)
{
    if (!g_n)
        return;
    if (R_SUCCEEDED(fsFileWrite(&g_file, g_off, g_buf, g_n * 4, FsWriteOption_None)))
        g_off += (s64)(g_n * 4);
    g_n = 0;
}

static void sampler_main(void* unused)
{
    (void)unused;
    uint64_t last_flush = armGetSystemTick();
    for (;;)
    {
        svcSleepThread(1000000ll); /* 1 ms */
        if (R_FAILED(svcSetThreadActivity(g_game, ThreadActivity_Paused)))
            break; /* the game thread has gone */
        ThreadContext c;
        Result rc = svcGetThreadContext3(&c, g_game);
        svcSetThreadActivity(g_game, ThreadActivity_Runnable);
        if (R_FAILED(rc))
            continue;
        if (!g_stack_hi)
        {
            MemoryInfo mi;
            u32 pi;
            if (R_SUCCEEDED(svcQueryMemory(&mi, &pi, c.sp)))
                g_stack_lo = mi.addr, g_stack_hi = mi.addr + mi.size;
        }
        if (g_n + 1 + SAMPLE_DEPTH > BUF_WORDS)
            flush_buf();
        uint32_t* rec = &g_buf[g_n];
        uint32_t n = 0;
        rec[1 + n++] = rva(c.pc.x);
        rec[1 + n++] = rva(c.lr);
        /* the frame records: { previous x29, return address }, 16-byte aligned, in the stack, going up */
        uint64_t fp = c.fp;
        while (n < SAMPLE_DEPTH && fp >= c.sp && fp + 16 <= g_stack_hi && fp >= g_stack_lo && !(fp & 15))
        {
            const uint64_t* r = (const uint64_t*)(uintptr_t)fp;
            uint64_t next = r[0], ret = r[1];
            if (!ret)
                break;
            uint32_t a = rva(ret);
            if (a != rec[n]) /* (lr is often the first frame's return address too) */
                rec[1 + n++] = a;
            if (next <= fp)
                break;
            fp = next;
        }
        rec[0] = n;
        g_n += 1 + n;
        if (armTicksToNs(armGetSystemTick() - last_flush) > 1000000000ull)
            flush_buf(), fsFileFlush(&g_file), last_flush = armGetSystemTick();
    }
    flush_buf();
    fsFileClose(&g_file);
}

int sampler_start(const char* path)
{
    /* sdmc:/a/b -> /a/b on the card's file system */
    const char* p = !strncmp(path, "sdmc:", 5) ? path + 5 : path;
    FsFileSystem* fs = fsdevGetDeviceFileSystem("sdmc");
    if (!fs)
        return 0;
    char fpath[FS_MAX_PATH];
    snprintf(fpath, sizeof fpath, "%s", p);
    fsFsDeleteFile(fs, fpath);
    if (R_FAILED(fsFsCreateFile(fs, fpath, 0, 0)) || R_FAILED(fsFsOpenFile(fs, fpath, FsOpenMode_Write | FsOpenMode_Append, &g_file)))
        return 0;
    static const char magic[8] = { 'F', 'X', 'S', 'A', 'M', 'P', 'L', '1' };
    fsFileWrite(&g_file, 0, magic, 8, FsWriteOption_Flush);
    g_off = 8;
    /* this thread's own handle (the caller is the game thread) */
    g_game = threadGetCurHandle();
    /* above the game's priority, on the core the game thread isn't on (it has core 0; the pool takes 1 and 2) */
    if (R_FAILED(threadCreate(&g_thread, sampler_main, NULL, NULL, 0x10000, 0x2A, 2)) || R_FAILED(threadStart(&g_thread)))
    {
        fsFileClose(&g_file);
        return 0;
    }
    return 1;
}
