/* The platform layer on the Nintendo Switch (plat_posix.c's __SWITCH__ parts, gwin.c's 2 GB window),
 * on the console or in an emulator: tools/build_switch.py plat-test builds it as an .nro.
 *
 * It checks what the game needs from the OS before any of the game is built: the guest window can be
 * reserved; commit gives zeroed, writable pages and keeps the contents of pages already committed;
 * decommit + commit gives zeroed pages back; how much the guest can commit at most; waiting on an
 * address and waking it from another thread; files and directories on the SD card; time and memory
 * figures. The results go to the screen and to sdmc:/switch/xi-on-anything/plat-test.log, one PASS or
 * FAIL line per check and a summary. + exits. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <switch.h>

#include "gwin.h"
#include "guest.h"
#include "plat.h"
#include "runtime.h"

#define DIR_ROOT "sdmc:/switch/xi-on-anything"
#define MB (1024u * 1024u)

int plat_physical_memory(void); /* plat_posix.c: commits map physical memory (else the window is heap) */
extern char* fake_heap_start;
extern char* fake_heap_end;

static FILE* g_log;
static int g_failed, g_passed;

static void say(const char* fmt, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    fputs(line, stdout);
    if (g_log)
    {
        fputs(line, g_log);
        fflush(g_log);
    }
    consoleUpdate(NULL);
}

static void check(int ok, const char* what)
{
    say("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (ok)
        ++g_passed;
    else
        ++g_failed;
}

static int all_bytes(const unsigned char* p, size_t n, unsigned char v)
{
    for (size_t i = 0; i < n; ++i)
        if (p[i] != v)
            return 0;
    return 1;
}

/* --- memory ----------------------------------------------------------------------------------------- */
static void test_memory(void)
{
    uint64_t total = 0, avail = 0;
    plat_memory(&total, &avail);
    say("memory: total %llu MB, available %llu MB, C heap %zu MB; guest window: %s\n", (unsigned long long)(total / MB),
        (unsigned long long)(avail / MB), (size_t)(fake_heap_end - fake_heap_start) / MB,
        plat_physical_memory() ? "physical (svcMapPhysicalMemory in the alias region)" : "heap (no system resource)");

    check(gwin_init(), "gwin_init reserves the 2 GB guest window");
    if (!rt_guest_base)
        return;
    say("guest window at %p\n", (void*)rt_guest_base);

    /* 64 MB: committed pages are zero and writable */
    uint32_t a = gwin_reserve(0, 64 * MB);
    check(a != 0, "gwin_reserve 64 MB");
    if (!a)
        return;
    int ok = gwin_commit(a, 64 * MB);
    check(ok, "gwin_commit 64 MB");
    if (!ok)
    {
        uint64_t alias = 0, alias_size = 0, sysres = 0;
        svcGetInfo(&alias, InfoType_AliasRegionAddress, CUR_PROCESS_HANDLE, 0);
        svcGetInfo(&alias_size, InfoType_AliasRegionSize, CUR_PROCESS_HANDLE, 0);
        svcGetInfo(&sysres, InfoType_SystemResourceSizeTotal, CUR_PROCESS_HANDLE, 0);
        Result rc = svcMapPhysicalMemory(GUEST_PTR(a), 0x1000);
        say("alias region 0x%llx + 0x%llx, system resource %llu KB, svcMapPhysicalMemory(4 KB) = 0x%x (module %u, "
            "description %u)\n",
            (unsigned long long)alias, (unsigned long long)alias_size, (unsigned long long)(sysres / 1024), rc,
            R_MODULE(rc), R_DESCRIPTION(rc));
        return;
    }
    check(all_bytes(GUEST_PTR(a), 64 * MB, 0), "committed pages read as zero");
    memset(GUEST_PTR(a), 0xA5, 64 * MB);
    check(all_bytes(GUEST_PTR(a), 64 * MB, 0xA5), "committed pages hold what is written");

    /* committing a range again keeps what its committed pages hold */
    gwin_decommit(a + 32 * MB, 32 * MB);
    ok = gwin_commit(a, 64 * MB);
    check(ok, "gwin_commit over a partly committed range");
    check(ok && all_bytes(GUEST_PTR(a), 32 * MB, 0xA5), "the committed half keeps its contents");
    check(ok && all_bytes(GUEST_PTR(a + 32 * MB), 32 * MB, 0), "the recommitted half is zero again");

    /* decommit + commit: zero again */
    gwin_decommit(a, 64 * MB);
    ok = gwin_commit(a, 64 * MB);
    check(ok && all_bytes(GUEST_PTR(a), 64 * MB, 0), "decommit + commit gives zeroed pages");
    gwin_decommit(a, 64 * MB);
    gwin_release(a);

    /* a single page at the top of the window, and the guard page beyond it is never needed */
    uint32_t top = gwin_reserve(0x7FF00000u, 0x10000u);
    check(top == 0x7FF00000u, "a reservation at the top of the 2 GB window");
    if (top && gwin_commit(top, 0x10000u))
    {
        *GUEST_PTR(top + 0xFFFF) = 1;
        check(*GUEST_PTR(top + 0xFFFF) == 1, "its last byte is writable");
        gwin_decommit(top, 0x10000u);
    }
    if (top)
        gwin_release(top);

    /* how much the guest can have committed at once: 16 MB at a time until commit fails */
    enum { STEP = 16 * MB, MAXN = 120 };
    uint32_t got[MAXN];
    unsigned n = 0;
    for (; n < MAXN; ++n)
    {
        uint32_t r = gwin_reserve(0, STEP);
        if (!r)
            break;
        if (!gwin_commit(r, STEP))
        {
            gwin_release(r);
            break;
        }
        GUEST_PTR(r)[0] = 1; /* touch it */
        got[n] = r;
    }
    say("the guest committed %u MB before commit failed%s\n", n * (STEP / MB), n == MAXN ? " (stopped at the test's limit)" : "");
    check(n * (STEP / MB) >= 1024, "the guest can commit at least 1 GB");
    GwinStats s;
    gwin_stats(&s);
    say("gwin_stats: committed %llu MB, peak %llu MB, top 0x%08x\n", (unsigned long long)(s.committed / MB),
        (unsigned long long)(s.committed_peak / MB), s.top);
    for (unsigned i = 0; i < n; ++i)
    {
        gwin_decommit(got[i], STEP);
        gwin_release(got[i]);
    }
    plat_memory(&total, &avail);
    say("after release: available %llu MB\n", (unsigned long long)(avail / MB));
}

/* --- waiting ---------------------------------------------------------------------------------------- */
static volatile uint32_t g_flag, g_seen;

static void waker(void* arg)
{
    (void)arg;
    plat_sleep_ms(50);
    plat_atomic_add32(&g_flag, 1);
    plat_wake_all32(&g_flag);
    g_seen = plat_thread_id();
}

static void test_wait(void)
{
    uint32_t me = plat_thread_id();
    check(me != 0, "plat_thread_id is non-zero");
    uint64_t t0 = rt_monotonic_ns();
    check(plat_thread_start(waker, NULL), "plat_thread_start");
    while (g_flag == 0)
        plat_wait32(&g_flag, 0);
    uint64_t waited = (rt_monotonic_ns() - t0) / 1000000;
    check(g_flag == 1, "plat_wait32 returns once another thread changes the value and wakes it");
    say("woken after %llu ms\n", (unsigned long long)waited);
    plat_sleep_ms(20);
    check(g_seen != 0 && g_seen != me, "the other thread has its own id");

    volatile uint32_t never = 0;
    t0 = rt_monotonic_ns();
    plat_wait32_ms(&never, 0, 100);
    waited = (rt_monotonic_ns() - t0) / 1000000;
    check(waited >= 90 && waited < 1000, "plat_wait32_ms times out after about 100 ms");

    volatile uint32_t v = 5;
    check(plat_atomic_cas32(&v, 5, 7) == 5 && v == 7, "plat_atomic_cas32");
    check(plat_atomic_add32(&v, 3) == 7 && v == 10, "plat_atomic_add32");
}

/* --- files ------------------------------------------------------------------------------------------ */
static void test_files(void)
{
    const char* dir = DIR_ROOT "/plat-test.d";
    const char* file = DIR_ROOT "/plat-test.d/file.bin";
    plat_unlink(file);
    plat_rmdir(dir);
    check(plat_mkdir(dir), "plat_mkdir on the SD card");
    PlatFile* f = plat_file_open(file, PLAT_WRITE | PLAT_CREATE | PLAT_TRUNCATE);
    check(f != NULL, "plat_file_open to write");
    if (!f)
        return;
    static unsigned char buf[256 * 1024];
    for (size_t i = 0; i < sizeof buf; ++i)
        buf[i] = (unsigned char)(i * 7);
    check(plat_file_write(f, buf, sizeof buf) == (int64_t)sizeof buf, "plat_file_write 256 KB");
    check(plat_file_size(f) == (int64_t)sizeof buf, "plat_file_size");
    plat_file_close(f);

    size_t n = 0;
    unsigned char* back = plat_read_file(file, &n);
    check(back && n == sizeof buf && memcmp(back, buf, n) == 0, "plat_read_file reads it back");
    free(back);

    PlatStat st;
    check(plat_stat(file, &st) && !st.is_dir && st.size == sizeof buf, "plat_stat on the file");
    check(plat_stat(dir, &st) && st.is_dir, "plat_stat on the directory");
    say("file mtime %llu (ms since 1970)\n", (unsigned long long)st.mtime_ms);

    PlatDir* d = plat_dir_open(dir);
    int found = 0;
    for (const char* e; d && (e = plat_dir_next(d));)
        found += strcmp(e, "file.bin") == 0;
    if (d)
        plat_dir_close(d);
    check(found == 1, "plat_dir_open lists the file");

    check(!plat_stat(DIR_ROOT "/no-such-file", &st) && plat_error() == PLAT_NOT_FOUND, "a missing file is PLAT_NOT_FOUND");
    check(plat_unlink(file) && plat_rmdir(dir), "plat_unlink and plat_rmdir");
}

/* --- network ---------------------------------------------------------------------------------------- */
/* sdmc:/switch/xi-on-anything/net-test.host: "name port" (a LandSandBoat server's sign-in, 54231). The
 * name is resolved and a TCP connection opened and closed: nothing is sent. Without the file, skipped. */
static void test_net(void)
{
    FILE* f = fopen(DIR_ROOT "/net-test.host", "r");
    if (!f)
    {
        say("network: skipped (no net-test.host)\n");
        return;
    }
    char host[256] = "";
    int port = 54231;
    if (fscanf(f, "%255s %d", host, &port) < 1)
        host[0] = 0;
    fclose(f);
    if (!host[0])
        return;
    Result rc = socketInitializeDefault();
    check(R_SUCCEEDED(rc), "socketInitializeDefault");
    if (R_FAILED(rc))
        return;
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char ports[16];
    snprintf(ports, sizeof ports, "%d", port);
    int ok = getaddrinfo(host, ports, &hints, &res) == 0 && res;
    check(ok, "getaddrinfo resolves the server");
    if (ok)
    {
        struct sockaddr_in* a = (struct sockaddr_in*)res->ai_addr;
        say("%s is %s\n", host, inet_ntoa(a->sin_addr));
        int s = socket(AF_INET, SOCK_STREAM, 0);
        uint64_t t0 = rt_monotonic_ns();
        int c = s >= 0 && connect(s, res->ai_addr, res->ai_addrlen) == 0;
        check(c, "a TCP connection to the server's sign-in port");
        if (c)
            say("connected in %llu ms\n", (unsigned long long)((rt_monotonic_ns() - t0) / 1000000));
        if (s >= 0)
            close(s);
        freeaddrinfo(res);
    }
    socketExit();
}

/* --- time ------------------------------------------------------------------------------------------- */
static void test_time(void)
{
    uint64_t now = plat_wall_ms();
    PlatTime t;
    plat_split_time(now, 1, &t);
    say("local time %04d-%02d-%02d %02d:%02d:%02d, UTC bias %d minutes\n", t.year, t.month, t.day, t.hour, t.minute,
        t.second, plat_utc_bias_minutes());
    check(t.year >= 2024, "plat_wall_ms is a real date");
    uint64_t a = rt_monotonic_ns();
    plat_sleep_ms(30);
    uint64_t b = rt_monotonic_ns();
    check(b - a >= 25000000ull, "rt_monotonic_ns advances across plat_sleep_ms");
}

int main(int argc, char** argv)
{
    (void)argc;
    (void)argv;
    consoleInit(NULL);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);

    mkdir("sdmc:/switch", 0777);
    mkdir(DIR_ROOT, 0777);
    g_log = fopen(DIR_ROOT "/plat-test.log", "w");
    say("XI on Anything: Switch platform test\n");

    test_time();
    test_files();
    test_wait();
    test_net();
    test_memory();

    say("\n%d passed, %d failed: %s\n", g_passed, g_failed, g_failed ? "FAILED" : "ALL PASSED");
    say("press + to exit\n");
    if (g_log)
        fclose(g_log);
    g_log = NULL;

    while (appletMainLoop())
    {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus)
            break;
        consoleUpdate(NULL);
    }
    consoleExit(NULL);
    return g_failed ? 1 : 0;
}
