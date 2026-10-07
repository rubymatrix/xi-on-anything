/* Optional frame timestamps for the Android performance runs (host/benchmark.c). Off by default;
 * no draw or API profiling. */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /* FFXI_BENCH_DIR enables the collector. The directory must exist; frames.csv and
     * markers.jsonl must not.
     * Returns 0 disabled, 1 enabled, -1 failed. Call once before the game starts. */
    int benchmark_init(void);
    /* At the start of the host's Present hook. Costs one branch when disabled. */
    void benchmark_frame(void);
    /* Flush and close at host cleanup; also registered with atexit. */
    void benchmark_close(void);
    int benchmark_enabled(void);

    /* Exported for LuaJIT's ffi. Labels and phases are ASCII letters, digits, spaces and _.-:
     * (under 160 characters); anything else is rejected and not written. Marks use the same
     * monotonic clock and UTC anchor as the frame timestamps. */
    int FFXI_BenchmarkMark(const char* label, const char* phase, uint32_t zone, uint32_t fixture_count);
    void FFXI_BenchmarkSetZone(uint32_t zone);

#ifdef __cplusplus
}
#endif
