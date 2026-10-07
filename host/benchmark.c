/* Frame-time collector for the local Android performance runs (benchmark.h): one monotonic
 * timestamp and one buffered numeric record per Present. It records no credentials, arguments,
 * readbacks or per-draw data. */
#include "benchmark.h"
#include "plat.h"
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#if defined(__GNUC__)
#define BENCH_EXPORT __attribute__((visibility("default"), used))
#else
#define BENCH_EXPORT
#endif

static PlatFile *frames, *markers;
static uint64_t anchor_ns, anchor_ms, previous_ns, flush_ns, frame_count;
static uint32_t current_zone;
static char buffer[65536];
static uint32_t buffered;
static int active, initialized, registered;

static int append(PlatFile* file, const char* bytes, uint32_t count)
{
    return file && plat_file_write(file, bytes, count) == (int64_t)count;
}

static int flush_frames(void)
{
    if (!buffered)
        return 1;
    if (!append(frames, buffer, buffered))
        return 0;
    buffered = 0;
    return 1;
}

void benchmark_close(void)
{
    active = 0;
    if (frames)
    {
        flush_frames();
        plat_file_flush(frames);
        plat_file_close(frames);
        frames = NULL;
    }
    if (markers)
    {
        plat_file_flush(markers);
        plat_file_close(markers);
        markers = NULL;
    }
}

static void failed(void)
{
    rt_log("[benchmark] output failed; timestamps are incomplete and cannot be admitted\n");
    benchmark_close();
}

int benchmark_init(const char* account)
{
    if (initialized)
        return active ? 1 : 0;
    initialized = 1;
    const char* dir = getenv("FFXI_BENCH_DIR");
    if (!dir || !*dir)
        return 0;
    if (!account || strcmp(account, "hxitest"))
    {
        rt_log("[benchmark] only the explicitly supplied local hxitest account is admitted\n");
        return -1;
    }
    char frame_path[2048], marker_path[2048];
    int nf = snprintf(frame_path, sizeof frame_path, "%s/frames.csv", dir);
    int nm = snprintf(marker_path, sizeof marker_path, "%s/markers.jsonl", dir);
    if (nf < 0 || nf >= (int)sizeof frame_path || nm < 0 || nm >= (int)sizeof marker_path)
        return -1;
    frames = plat_file_open(frame_path, PLAT_WRITE | PLAT_CREATE | PLAT_EXCL);
    if (!frames)
    {
        rt_log("[benchmark] frames.csv must be absent and its directory writable\n");
        return -1;
    }
    markers = plat_file_open(marker_path, PLAT_WRITE | PLAT_CREATE | PLAT_EXCL);
    if (!markers)
    {
        plat_file_close(frames);
        frames = NULL;
        /* This invocation exclusively created this still-empty file. */
        plat_unlink(frame_path);
        rt_log("[benchmark] markers.jsonl must be absent and its directory writable\n");
        return -1;
    }
    anchor_ns = rt_monotonic_ns();
    anchor_ms = plat_wall_ms();
    previous_ns = flush_ns = anchor_ns;
    frame_count = buffered = current_zone = 0;
    static const char header[] = "epoch,frame_ms,zone,frame,monotonic_ns\n";
    if (!append(frames, header, sizeof header - 1))
    {
        failed();
        return -1;
    }
    active = 1;
    if (!registered)
        atexit(benchmark_close), registered = 1;
    return FFXI_BenchmarkMark("collector initialized", "setup", 0, 0) ? 1 : -1;
}

int benchmark_enabled(void)
{
    return active;
}

void benchmark_frame(void)
{
    if (!active)
        return;
    uint64_t now = rt_monotonic_ns();
    if (now < previous_ns || now < anchor_ns)
    {
        failed();
        return;
    }
    double epoch = (double)anchor_ms / 1000.0 + (double)(now - anchor_ns) / 1e9;
    double dt = (double)(now - previous_ns) / 1e6;
    char row[192];
    int n = snprintf(row, sizeof row, "%.6f,%.6f,%u,%" PRIu64 ",%" PRIu64 "\n", epoch, dt, current_zone, frame_count++,
                     now);
    if (n < 0 || n >= (int)sizeof row || (buffered + (uint32_t)n > sizeof buffer && !flush_frames()))
    {
        failed();
        return;
    }
    memcpy(buffer + buffered, row, (size_t)n);
    buffered += (uint32_t)n;
    previous_ns = now;
    if (now - flush_ns >= 1000000000ull)
    {
        if (!flush_frames())
            failed();
        flush_ns = now;
    }
}

static int safe_label(const char* s)
{
    if (!s)
        return 0;
    for (unsigned n = 0; n < 160; ++n)
    {
        unsigned char c = (unsigned char)s[n];
        if (!c)
            return 1;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' || c == '_' ||
              c == '.' || c == '-' || c == ':'))
            return 0;
    }
    return 0;
}

BENCH_EXPORT int FFXI_BenchmarkMark(const char* label, const char* phase, uint32_t zone, uint32_t fixture_count)
{
    if (!active || !safe_label(label) || !safe_label(phase) || zone > 65535 || fixture_count > 2304)
        return 0;
    uint64_t now = rt_monotonic_ns();
    if (now < anchor_ns)
        return 0;
    current_zone = zone;
    char row[768];
    double epoch = (double)anchor_ms / 1000.0 + (double)(now - anchor_ns) / 1e9;
    int n = snprintf(row, sizeof row,
                     "{\"epoch\":%.6f,\"monotonic_ns\":%" PRIu64 ",\"label\":\"%s\",\"phase\":\"%s\","
                     "\"zone\":%u,\"fixture_count\":%u,\"frame\":%" PRIu64 "}\n",
                     epoch, now, label, phase, zone, fixture_count, frame_count);
    if (n < 0 || n >= (int)sizeof row || !flush_frames() || !append(markers, row, (uint32_t)n))
    {
        failed();
        return 0;
    }
    return 1;
}

BENCH_EXPORT void FFXI_BenchmarkSetZone(uint32_t zone)
{
    if (zone <= 65535)
        current_zone = zone;
}
