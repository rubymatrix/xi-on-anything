/* host/benchmark.c with a stub clock and real files: enabling, refusals, failures and the records.
 *
 *   benchmark_test off|account|existing-frame|existing-marker|write-fail|clock-back|records <dir>
 *
 * Built and run by tests/android_policy_test.py. */
#include "benchmark.h"
#include "plat.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>

struct PlatFile
{
    int fd;
};
static uint64_t ticks = 1000000000ull;
static unsigned clock_reads;
static int fail_write;
uint64_t rt_monotonic_ns(void)
{
    ++clock_reads;
    return ticks;
}
uint64_t plat_wall_ms(void)
{
    return 1791000000123ull;
}
void rt_log(const char* f, ...)
{
    (void)f;
}
PlatFile* plat_file_open(const char* path, int flags)
{
    int fd = open(path, O_WRONLY | ((flags & PLAT_CREATE) ? O_CREAT : 0) | ((flags & PLAT_EXCL) ? O_EXCL : 0), 0600);
    if (fd < 0)
        return NULL;
    PlatFile* file = malloc(sizeof *file);
    file->fd = fd;
    return file;
}
int64_t plat_file_write(PlatFile* f, const void* data, uint32_t n)
{
    return fail_write ? -1 : write(f->fd, data, n);
}
int plat_file_flush(PlatFile* f)
{
    return fsync(f->fd) == 0;
}
void plat_file_close(PlatFile* f)
{
    close(f->fd);
    free(f);
}
int plat_unlink(const char* path)
{
    return unlink(path) == 0;
}
static void path(char* out, size_t capacity, const char* dir, const char* name)
{
    int n = snprintf(out, capacity, "%s/%s", dir, name);
    assert(n >= 0 && (size_t)n < capacity);
}
static void original(const char* p)
{
    FILE* f = fopen(p, "wb");
    assert(f);
    fputs("original\n", f);
    fclose(f);
}
static void retained(const char* p)
{
    char b[30];
    FILE* f = fopen(p, "rb");
    assert(f);
    assert(fgets(b, sizeof b, f));
    assert(!strcmp(b, "original\n"));
    fclose(f);
}
int main(int argc, char** argv)
{
    assert(argc == 3);
    const char* mode = argv[1];
    char frame[2048], mark[2048];
    path(frame, sizeof frame, argv[2], "frames.csv");
    path(mark, sizeof mark, argv[2], "markers.jsonl");
    if (!strcmp(mode, "off"))
    {
        unsetenv("FFXI_BENCH_DIR");
        assert(benchmark_init(NULL) == 0);
        for (unsigned i = 0; i < 100000; i++)
            benchmark_frame();
        assert(clock_reads == 0 && !benchmark_enabled());
    }
    else
    {
        setenv("FFXI_BENCH_DIR", argv[2], 1);
        if (!strcmp(mode, "account"))
        {
            assert(benchmark_init("other") == -1);
            assert(access(frame, F_OK) != 0 && access(mark, F_OK) != 0);
        }
        else if (!strcmp(mode, "existing-frame"))
        {
            original(frame);
            assert(benchmark_init("hxitest") == -1);
            retained(frame);
            assert(access(mark, F_OK) != 0);
        }
        else if (!strcmp(mode, "existing-marker"))
        {
            original(mark);
            assert(benchmark_init("hxitest") == -1);
            retained(mark);
            assert(access(frame, F_OK) != 0);
        }
        else if (!strcmp(mode, "write-fail"))
        {
            assert(benchmark_init("hxitest") == 1);
            fail_write = 1;
            ticks += 2000000000ull;
            benchmark_frame();
            assert(!benchmark_enabled());
        }
        else if (!strcmp(mode, "clock-back"))
        {
            assert(benchmark_init("hxitest") == 1);
            --ticks;
            benchmark_frame();
            assert(!benchmark_enabled());
        }
        else if (!strcmp(mode, "records"))
        {
            assert(benchmark_init("hxitest") == 1);
            assert(FFXI_BenchmarkMark("stress phase start", "mixed-32", 234, 32));
            assert(!FFXI_BenchmarkMark("bad\"label", "mixed-32", 234, 32));
            assert(!FFXI_BenchmarkMark(NULL, "mixed-32", 234, 32));
            for (unsigned i = 0; i < 7000; i++)
            {
                ticks += 16666667;
                benchmark_frame();
            }
            assert(FFXI_BenchmarkMark("stress phase end", "mixed-32", 234, 32));
            benchmark_close();
            FILE* f = fopen(frame, "r");
            char line[1024];
            assert(f && fgets(line, sizeof line, f));
            assert(!strcmp(line, "epoch,frame_ms,zone,frame,monotonic_ns\n"));
            for (unsigned i = 0; i < 7000; i++)
            {
                double epoch, ms;
                unsigned zone;
                unsigned long long number, ns;
                assert(fgets(line, sizeof line, f));
                assert(sscanf(line, "%lf,%lf,%u,%llu,%llu", &epoch, &ms, &zone, &number, &ns) == 5);
                assert(zone == 234 && number == i && ns == 1000000000ull + (i + 1) * 16666667ull);
                assert(ms > 16.666 && ms < 16.667 && epoch > 1791000000.123);
            }
            assert(!fgets(line, sizeof line, f));
            fclose(f);
            f = fopen(mark, "r");
            assert(f);
            unsigned count = 0;
            while (fgets(line, sizeof line, f))
                ++count;
            fclose(f);
            assert(count == 3);
        }
        else
            assert(!"unknown case");
    }
    benchmark_close();
    benchmark_close();
    puts("PASS");
    return 0;
}
