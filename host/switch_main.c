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
    for (int i = 1; i + 1 < n;)
    {
        if (!strcmp(args[i], "--switch-layout"))
        {
            nx_set_layout(!strcmp(args[i + 1], "xbox"));
            memmove(args + i, args + i + 2, (size_t)(n - i - 2) * sizeof *args);
            n -= 2;
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
    int ret = xi_host_main(n, args);
    fprintf(stderr, "[switch] host exit %d\n", ret);
    appletUnhook(&g_hook);
    socketExit();
    fflush(NULL);
    return ret;
}
