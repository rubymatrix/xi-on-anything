/* Entry point of the Nintendo Switch build (docs/platforms/switch.md): libnx's main. It brings up the
 * system services the game needs (sockets for the servers, the screen's size), adds the arguments in
 * sdmc:/switch/xi-on-anything/run.args, finds the game folder tools/build_switch.py setup copied beside
 * them, sends the log to host64.log there, then starts the portable host (host64.c, built with main
 * renamed to xi_host_main), as host/ios_main.c and host/android_main.c do.
 *
 * Switch-only options, taken out before host64 sees the command line:
 *   --switch-layout nintendo|xbox   the face buttons (sdl_nx.c): A confirms (the default), or B by position
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <switch.h>

#include "nx_bridge.h"

int xi_host_main(int argc, char** argv);

#define APP_DIR "sdmc:/switch/xi-on-anything"
#define MAX_ARGS 64

/* The sockets' buffers: the game keeps a few TCP connections (lobby, map, sign-in) and one UDP socket;
 * libnx's defaults are for a couple of small transfers. */
static const SocketInitConfig SOCKETS = {
    .tcp_tx_buf_size = 0x8000,
    .tcp_rx_buf_size = 0x10000,
    .tcp_tx_buf_max_size = 0x40000,
    .tcp_rx_buf_max_size = 0x40000,
    .udp_tx_buf_size = 0x2400,
    .udp_rx_buf_size = 0xA500,
    .sb_efficiency = 8,
    .num_bsd_sessions = 8,
    .bsd_service_type = BsdServiceType_Auto,
};

/* --- the CPU's clock -----------------------------------------------------------------------------------
 * The game's own thread is what limits the frame rate (26 ms a frame at the 1020 MHz games run at;
 * docs/switch.md). Two ways past it, both undone when the app ends:
 *   - a zone change (no frame for 300 ms: a watcher thread, as the load is one long frame) turns on the
 *     system's boost for loading (appletSetCpuBoostMode FastLoad: the CPU at 1785 MHz, the GPU at its least),
 *     off again after 5 frames (the GPU's least must not last into play); FFXI_SWITCH_LOADBOOST=0 leaves it off
 *   - FFXI_SWITCH_CPU=<MHz> (run.args, e.g. FFXI_SWITCH_CPU=1785) holds the CPU at that clock through clkrst
 *     (sys-clk's way; Atmosphère's homebrew loader has the permission), set again every few seconds as the
 *     system's performance modes (dock, sleep) put theirs back. More frames, less battery, a warmer console. */
static int g_clk_mhz;
static ClkrstSession g_clk;
static int g_clk_open;
static u32 g_clk_was;

static void clock_start(void)
{
    const char* e = getenv("FFXI_SWITCH_CPU");
    g_clk_mhz = e ? atoi(e) : 0;
    if (g_clk_mhz <= 0)
        return;
    Result rc = clkrstInitialize();
    if (R_SUCCEEDED(rc))
        rc = clkrstOpenSession(&g_clk, PcvModuleId_CpuBus, 3);
    if (R_FAILED(rc))
    {
        fprintf(stderr, "[switch] FFXI_SWITCH_CPU: no clock control (clkrst 0x%x); the CPU stays at the system's\n", rc);
        g_clk_mhz = 0;
        return;
    }
    g_clk_open = 1;
    clkrstGetClockRate(&g_clk, &g_clk_was);
    rc = clkrstSetClockRate(&g_clk, (u32)g_clk_mhz * 1000000u);
    u32 now = 0;
    clkrstGetClockRate(&g_clk, &now);
    fprintf(stderr, "[switch] CPU clock: %u MHz, was %u MHz (set: 0x%x)\n", now / 1000000u, g_clk_was / 1000000u, rc);
}

/* the loading boost's watcher: it logs nothing (another thread's stdio breaks the main thread's); the game's
 * thread logs its turns (switch_clock_frame) */
static Thread g_watch;
static volatile int g_watch_run, g_boost_on;
static volatile uint64_t g_last_present_ns;
static volatile uint32_t g_presents, g_boost_turns, g_boost_failed;

static void boost_watch(void* arg)
{
    (void)arg;
    uint32_t at = 0;
    while (g_watch_run)
    {
        svcSleepThread(50000000ll);
        uint64_t last = g_last_present_ns, now = armTicksToNs(armGetSystemTick());
        if (!last)
            continue; /* the game not drawing yet: the sign-in screen's own frames aren't counted */
        if (!g_boost_on && now - last > 300000000ull)
        {
            if (R_FAILED(appletSetCpuBoostMode(ApmCpuBoostMode_FastLoad)))
            {
                g_boost_failed = 1;
                return;
            }
            g_boost_on = 1, at = g_presents, g_boost_turns++;
        }
        else if (g_boost_on && g_presents - at >= 5)
        {
            appletSetCpuBoostMode(ApmCpuBoostMode_Normal);
            g_boost_on = 0, g_boost_turns++;
        }
    }
}

static void boost_start(void)
{
    const char* e = getenv("FFXI_SWITCH_LOADBOOST");
    if ((e && *e == '0') || g_clk_mhz >= 1785) /* a CPU held at 1785 has nothing to gain */
        return;
    g_watch_run = 1;
    if (R_FAILED(threadCreate(&g_watch, boost_watch, NULL, NULL, 0x4000, 0x2C, -2)) || R_FAILED(threadStart(&g_watch)))
        g_watch_run = 0;
}

static void clock_end(void)
{
    if (g_watch_run)
    {
        g_watch_run = 0;
        threadWaitForExit(&g_watch);
        threadClose(&g_watch);
    }
    if (g_boost_on)
        appletSetCpuBoostMode(ApmCpuBoostMode_Normal), g_boost_on = 0;
    if (g_clk_open)
    {
        if (g_clk_was)
            clkrstSetClockRate(&g_clk, g_clk_was);
        clkrstCloseSession(&g_clk);
        clkrstExit();
        g_clk_open = 0;
    }
}

/* FFXI_PROFILE: the CPU's, GPU's and memory's clocks every 10 s (what the system, sys-clk or Horizon OC set
 * them to: a GPU held low shows as Present's wait in the profile) */
static void clock_report(uint64_t now)
{
    static int state; /* 0 not tried, 1 open, -1 not available */
    static ClkrstSession s[3];
    static uint64_t at;
    if (state < 0 || (state && now - at < 10000000000ull))
        return;
    at = now;
    if (!state)
    {
        const char* e = getenv("FFXI_PROFILE");
        static const PcvModuleId ids[3] = { PcvModuleId_CpuBus, PcvModuleId_GPU, PcvModuleId_EMC };
        state = -1;
        if (!(e && *e && *e != '0') || R_FAILED(clkrstInitialize()))
            return;
        for (int i = 0; i < 3; ++i)
            if (R_FAILED(clkrstOpenSession(&s[i], ids[i], 3)))
            {
                while (i--)
                    clkrstCloseSession(&s[i]);
                clkrstExit();
                fprintf(stderr, "[switch] clocks: not readable\n");
                return;
            }
        state = 1;
    }
    u32 hz[3] = { 0 };
    for (int i = 0; i < 3; ++i)
        clkrstGetClockRate(&s[i], &hz[i]);
    fprintf(stderr, "[switch] clocks: CPU %u MHz, GPU %u MHz, memory %u MHz (%s)\n", hz[0] / 1000000u, hz[1] / 1000000u,
        hz[2] / 1000000u, appletGetOperationMode() == AppletOperationMode_Console ? "docked" : "handheld");
}

/* each frame, from host64.c's present hook (the game's thread) */
void switch_clock_frame(void)
{
    static uint64_t checked;
    static uint32_t said_turns;
    static int said_failed;
    uint64_t now = armTicksToNs(armGetSystemTick());
    g_last_present_ns = now;
    g_presents++;
    clock_report(now);
    if (g_boost_turns != said_turns)
    {
        said_turns = g_boost_turns;
        fprintf(stderr, "[switch] loading boost %s (%u turns)\n", g_boost_on ? "on" : "off", said_turns);
    }
    if (g_boost_failed && !said_failed)
        said_failed = 1, fprintf(stderr, "[switch] loading boost unavailable\n");
    if (g_clk_open && now - checked > 3000000000ull)
    {
        /* the system's performance modes (dock, sleep) put their clock back */
        checked = now;
        u32 hz = 0;
        if (R_SUCCEEDED(clkrstGetClockRate(&g_clk, &hz)) && hz / 1000000u != (u32)g_clk_mhz)
            clkrstSetClockRate(&g_clk, (u32)g_clk_mhz * 1000000u);
    }
}

/* run.args: one literal argument per line (no shell expansion), as on Android and iOS. */
static int read_args(const char* path, char** out, int room)
{
    FILE* f = fopen(path, "r");
    if (!f)
        return 0;
    int n = 0;
    char line[2048];
    while (n < room && fgets(line, sizeof line, f))
    {
        size_t len = strcspn(line, "\r\n");
        line[len] = 0;
        if (!strncmp(line, "FFXI_", 5) && strchr(line, '='))
        {
            /* FFXI_NAME=value: an environment variable (there is no shell to set one) */
            char* eq = strchr(line, '=');
            *eq = 0;
            setenv(line, eq + 1, 1);
            fprintf(stderr, "[switch] set %s=%s\n", line, eq + 1);
        }
        else if (len)
            out[n++] = strdup(line);
    }
    fclose(f);
    return n;
}

static int has_arg(int argc, char** argv, const char* name)
{
    for (int i = 1; i < argc; ++i)
        if (!strcmp(argv[i], name))
            return 1;
    return 0;
}

static int is_dir(const char* path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/* the screen: 1920x1080 on a TV, 1280x720 in the hand */
static void view_size(void)
{
    int docked = appletGetOperationMode() == AppletOperationMode_Console;
    nx_set_view_size(docked ? 1920 : 1280, docked ? 1080 : 720);
}

/* docked and undocked while the game runs (the applet's messages come to this hook, on the thread that
 * runs appletMainLoop: sdl_nx.c's input polling calls it through appletMainLoop's hook list) */
static AppletHookCookie g_hook;
static void applet_hook(AppletHookType type, void* param)
{
    (void)param;
    if (type == AppletHookType_OnOperationMode)
        view_size();
}

int main(int argc, char** argv)
{
    mkdir("sdmc:/switch", 0777);
    mkdir(APP_DIR, 0777);
    chdir(APP_DIR);
    /* emptied, then both streams opened to append: with stderr at its own offset ("w"), its lines and
     * stdout's (OutputDebugString) wrote over each other */
    FILE* empty = fopen(APP_DIR "/host64.log", "w");
    if (empty)
        fclose(empty);
    if (freopen(APP_DIR "/host64.log", "a", stderr))
        setvbuf(stderr, NULL, _IOLBF, 0);
    if (freopen(APP_DIR "/host64.log", "a", stdout))
        setvbuf(stdout, NULL, _IOLBF, 0);

    Result rc = socketInitialize(&SOCKETS);
    if (R_FAILED(rc))
        fprintf(stderr, "[switch] socketInitialize failed: 0x%x (no network)\n", rc);
    appletHook(&g_hook, applet_hook, NULL);
    view_size();
    /* the Home button suspends the game rather than ending it; the console's sleep ends the connections */
    appletSetFocusHandlingMode(AppletFocusHandlingMode_SuspendHomeSleepNotify);

    static char* args[MAX_ARGS];
    int n = 0;
    args[n++] = argc > 0 && argv[0] ? argv[0] : (char*)APP_DIR "/xi-on-anything.nro";
    for (int i = 1; i < argc && n < MAX_ARGS; ++i)
        args[n++] = argv[i];
    n += read_args(APP_DIR "/run.args", args + n, MAX_ARGS - 12 - n);

    /* the Switch's own options, out of the command line */
    for (int i = 1; i < n;)
    {
        if (!strcmp(args[i], "--switch-layout") && i + 1 < n)
        {
            nx_set_layout(!strcmp(args[i + 1], "xbox"));
            memmove(args + i, args + i + 2, (size_t)(n - i - 2) * sizeof *args);
            n -= 2;
            continue;
        }
        /* an environment setting, NAME=VALUE (FFXI_FRAMELOG=1: the frame rate in host64.log each second). A
         * line FFXI_NAME=value alone does the same (read_args); a --switch-env left without its setting (its
         * FFXI_ line taken by read_args) is dropped, not handed to host64, which stops at an unknown option */
        if (!strcmp(args[i], "--switch-env"))
        {
            int has = i + 1 < n && strchr(args[i + 1], '=') && args[i + 1][0] != '-';
            if (has)
            {
                putenv(args[i + 1]);
                fprintf(stderr, "[switch] set %s\n", args[i + 1]);
            }
            memmove(args + i, args + i + 1 + has, (size_t)(n - i - 1 - has) * sizeof *args);
            n -= 1 + has;
            continue;
        }
        ++i;
    }

    static char game[] = APP_DIR "/SquareEnix/FINAL FANTASY XI";
    if (!has_arg(n, args, "--game") && is_dir(game))
    {
        args[n++] = "--game";
        args[n++] = game;
    }
    if (!has_arg(n, args, "--game"))
        fprintf(stderr, "[switch] no game folder: copy it with tools/build_switch.py setup --game (to %s)\n", game);
    static char reg[] = APP_DIR "/ffxi.reg", overlay[] = APP_DIR "/settings.reg", data[] = APP_DIR "/data";
    struct stat st;
    if (!has_arg(n, args, "--reg") && stat(reg, &st) == 0)
    {
        args[n++] = "--reg";
        args[n++] = reg;
        /* the Switch's defaults over the desktop's (switch/switch.reg: the screen's size, windowed), still
         * under what the player saves (the overlay) */
        static char sw_reg[] = APP_DIR "/switch.reg";
        if (stat(sw_reg, &st) == 0)
        {
            args[n++] = "--reg";
            args[n++] = sw_reg;
        }
    }
    if (!has_arg(n, args, "--reg-overlay"))
    {
        args[n++] = "--reg-overlay";
        args[n++] = overlay;
    }
    if (!has_arg(n, args, "--data-dir"))
    {
        mkdir(data, 0777);
        args[n++] = "--data-dir";
        args[n++] = data;
    }
    /* no Discord to talk to; the scene effects and the frame-rate gauge off by default, as on iOS
     * (Config > Modern still turns them on) */
    setenv("FFXI_DISCORD", "0", 0);
    setenv("FFXI_FX", "0", 0);
    setenv("FFXI_FPS", "0", 0);
    args[n] = NULL;
    fprintf(stderr, "[switch] host64 with %d arguments:", n - 1);
    for (int i = 1; i < n; ++i)
        fprintf(stderr, " %s", i > 1 && (!strcmp(args[i - 1], "--pass") || !strcmp(args[i - 1], "--otp")) ? "(hidden)" : args[i]);
    fprintf(stderr, "\n");
    clock_start();
    boost_start();
    atexit(clock_end); /* the game may end with exit() too */
    int ret = xi_host_main(n, args);
    clock_end();
    fprintf(stderr, "[switch] host exit %d\n", ret);
    appletUnhook(&g_hook);
    socketExit();
    fflush(NULL);
    return ret;
}
