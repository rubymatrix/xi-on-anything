/* XI on Anything's settings for the Nintendo Switch (docs/platforms/switch.md, Settings): a homebrew app of
 * its own, xi-on-anything-settings.nro, that changes what the game reads when it starts, without a PC:
 *   - the display, in data/settings.reg (the file Config > Display writes, read last): the game's
 *     resolution (0001 x 0002), the interface's size (0037 x 0038, the menus' resolution) and the
 *     background's (0003 x 0004: the world's size before it is fitted to the window). The game on the
 *     Switch draws at the screen's size and its world at the screen's too unless run.args says otherwise
 *     (host64.c: FFXI_SWITCH_FIT=0, FFXI_SWITCH_BG=<size>), so a size chosen here writes that line too;
 *     "The screen's" (the first choice of each) takes it out
 *   - the rest, in run.args (one item a line; every line it doesn't set is kept as it is): the frame rate
 *     (--fps-divisor), the face buttons (--switch-layout), the CPU's clock (FFXI_SWITCH_LOADBOOST,
 *     FFXI_SWITCH_CPU) and what a lost connection does (FFXI_SWITCH_RELOGIN)
 * Then it can start the game (hbloader's next load: from the homebrew menu started over a game, the game
 * has its full memory). It holds no game code. Drawn on libnx's text console; the D-pad moves and changes,
 * A changes, + saves and starts the game, B leaves. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <switch.h>

#ifndef APP_DIR /* (a test on a PC gives a folder of its own) */
#define APP_DIR "sdmc:/switch/xi-on-anything"
#endif
#define GAME_NRO APP_DIR "/xi-on-anything.nro"
#define RUN_ARGS APP_DIR "/run.args"
#define DATA_REG APP_DIR "/data/settings.reg"
#define MAX_LINES 128
#define LINE_LEN 512

/* --- the choices ---------------------------------------------------------------------------------------- */
typedef struct
{
    const char* name;
    int w, h;
} Res;
static const Res RES[] = {
    { "The screen's (720p / 1080p docked)", 0, 0 },
    { "1920x1080", 1920, 1080 },
    { "1600x900", 1600, 900 },
    { "1280x720", 1280, 720 },
    { "960x540", 960, 540 },
    { "640x480 (4:3)", 640, 480 },
    { "512x448 (4:3, the PS2's)", 512, 448 },
};
#define NRES (int)(sizeof RES / sizeof RES[0])
static const float UI_SCALE[] = { 2.0f, 1.5f, 1.0f };
static const char* const UI_NAMES[] = { "Large (2x)", "Medium (1.5x)", "Small (1x)" };
static const int BG[] = { 0, 2048, 1024, 512 };
static const char* const BG_NAMES[] = { "The screen's", "2048 (sharpest)", "1024", "512 (soft, lightest)" };
static const char* const FPS_NAMES[] = { "Uncapped (up to 60)", "Uncapped, 30 when slow", "60", "30" };
static const char* const FPS_VALUES[] = { NULL, NULL, "1", "2" }; /* (the second: FFXI_FPS_ADAPT=1, uncapped) */
static const char* const LAYOUT_NAMES[] = { "Nintendo (A confirms)", "Xbox (B confirms)" };
static const char* const CPU_NAMES[] = { "Boost while loading", "No boost", "Full speed (warmer)" };
static const char* const RELOGIN_NAMES[] = { "Back to the sign-in screen", "Close the game" };
static const char* const CRT_NAMES[] = { "Off", "Scanlines", "CRT (scanlines and grille)" };
static const char* const CURVE_NAMES[] = { "Off", "On" };

enum
{
    O_RES,
    O_UI,
    O_BG,
    O_FPS,
    O_CRT,
    O_CURVE,
    O_LAYOUT,
    O_CPU,
    O_RELOGIN,
    NOPTS,
    A_START = NOPTS,
    A_SAVE,
    A_RESET,
    A_QUIT,
    NROWS
};
typedef struct
{
    const char* label;
    int n, cur;
    const char* const* names;
    const char* help;
} Opt;
static const char* g_res_names[NRES];
static Opt g_opt[NOPTS] = {
    { "Resolution", NRES, 0, g_res_names,
        "The size the game draws at, fitted to the screen. Lower is faster. 4:3 sizes get black bars at the sides." },
    { "Interface size", 3, 0, UI_NAMES, "How large the menus and text are. Large reads well on a TV and in handheld." },
    { "Background", 4, 0, BG_NAMES,
        "The world's resolution before it is fitted to the screen. Lower uses less memory and looks softer." },
    { "Frame rate", 4, 0, FPS_NAMES,
        "Uncapped keeps the world at its real speed at any frame rate (with this game version's frame sites). 30 when "
        "slow: an even 30 while frames can't keep above 40, uncapped again when they can." },
    { "CRT filter", 3, 0, CRT_NAMES,
        "An old TV's look: a scanline per row, and with CRT the screen's red, green and blue stripes. Best at 512x448 or 640x480, docked." },
    { "Screen curve", 2, 0, CURVE_NAMES, "With the CRT filter: the glass's curve and darker corners." },
    { "Buttons", 2, 0, LAYOUT_NAMES, "Which face button confirms." },
    { "CPU", 3, 0, CPU_NAMES,
        "Boost: full speed while a zone loads. No boost: for an overclocking tool (sys-clk). Full speed: more fps, less battery." },
    { "After a lost connection", 2, 0, RELOGIN_NAMES,
        "From the HOME menu icon, the game can restart at the sign-in screen instead of closing." },
};

/* --- run.args ------------------------------------------------------------------------------------------- */
static char g_args[MAX_LINES][LINE_LEN];
static int g_nargs;

static void trim(char* s)
{
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '\t'))
        s[--n] = 0;
    size_t i = 0;
    while (s[i] == ' ' || s[i] == '\t')
        ++i;
    if (i)
        memmove(s, s + i, n - i + 1);
}

static void args_read(void)
{
    g_nargs = 0;
    FILE* f = fopen(RUN_ARGS, "rb");
    if (!f)
        return;
    char line[LINE_LEN];
    while (g_nargs < MAX_LINES && fgets(line, sizeof line, f))
    {
        trim(line);
        if (line[0])
            snprintf(g_args[g_nargs++], LINE_LEN, "%s", line);
    }
    fclose(f);
}

/* the line after an option's (its value), or NULL */
static const char* args_value(const char* opt)
{
    for (int i = 0; i + 1 < g_nargs; ++i)
        if (!strcmp(g_args[i], opt))
            return g_args[i + 1];
    return NULL;
}

/* a NAME=value line's value, or NULL */
static const char* args_env(const char* name)
{
    size_t n = strlen(name);
    for (int i = 0; i < g_nargs; ++i)
        if (!strncmp(g_args[i], name, n) && g_args[i][n] == '=')
            return g_args[i] + n + 1;
    return NULL;
}

static void args_drop(int i, int count)
{
    memmove(g_args[i], g_args[i + count], (size_t)(g_nargs - i - count) * LINE_LEN);
    g_nargs -= count;
}

/* an option and its value out (every time it is there), then in again when value isn't NULL */
static void args_set(const char* opt, const char* value)
{
    for (int i = 0; i < g_nargs;)
        if (!strcmp(g_args[i], opt))
            args_drop(i, i + 1 < g_nargs ? 2 : 1);
        else
            ++i;
    if (value && g_nargs + 2 <= MAX_LINES)
    {
        snprintf(g_args[g_nargs++], LINE_LEN, "%s", opt);
        snprintf(g_args[g_nargs++], LINE_LEN, "%s", value);
    }
}

static void args_set_env(const char* name, const char* value)
{
    size_t n = strlen(name);
    for (int i = 0; i < g_nargs;)
        if (!strncmp(g_args[i], name, n) && g_args[i][n] == '=')
            args_drop(i, 1);
        else
            ++i;
    if (value && g_nargs < MAX_LINES)
        snprintf(g_args[g_nargs++], LINE_LEN, "%s=%s", name, value);
}

/* written beside, then put in its place (a crash part way leaves the old one) */
static int replace_file(const char* path, const char* tmp)
{
    remove(path);
    return rename(tmp, path) == 0;
}

static int args_write(void)
{
    char tmp[256];
    snprintf(tmp, sizeof tmp, "%s.new", RUN_ARGS);
    FILE* f = fopen(tmp, "wb");
    if (!f)
        return 0;
    for (int i = 0; i < g_nargs; ++i)
        fprintf(f, "%s\n", g_args[i]);
    if (fclose(f))
        return 0;
    return replace_file(RUN_ARGS, tmp);
}

/* --- the display's settings ----------------------------------------------------------------------------- */
static const char* const REG_NAMES[6] = { "0001", "0002", "0003", "0004", "0037", "0038" };

/* a dword the file has, or -1 */
static long reg_read(const char* path, const char* name)
{
    FILE* f = fopen(path, "rb");
    if (!f)
        return -1;
    char line[LINE_LEN];
    long v = -1;
    while (fgets(line, sizeof line, f))
        if (line[0] == '"' && !strncmp(line + 1, name, 4) && !strncmp(line + 5, "\"=dword:", 8))
            v = strtol(line + 13, NULL, 16);
    fclose(f);
    return v;
}

/* the value the game will start with: the player's settings (data/, then the game's own save), else the
 * Switch's defaults */
static long reg_value(const char* name, long fallback)
{
    static const char* const FILES[] = { DATA_REG, APP_DIR "/settings.reg", APP_DIR "/switch.reg" };
    for (int i = 0; i < 3; ++i)
    {
        long v = reg_read(FILES[i], name);
        if (v > 0)
            return v;
    }
    return fallback;
}

/* the values into data/settings.reg: its lines for them rewritten, the rest kept, any missing added (as
 * host/modern.c's save_display) */
static int reg_write(const long v[6])
{
    char tmp[256], line[LINE_LEN];
    snprintf(tmp, sizeof tmp, "%s.new", DATA_REG);
    mkdir(APP_DIR "/data", 0777);
    FILE* in = fopen(DATA_REG, "rb");
    FILE* out = fopen(tmp, "wb");
    if (!out)
    {
        if (in)
            fclose(in);
        return 0;
    }
    int written[6] = { 0 }, crlf = 1, any = 0;
    while (in && fgets(line, sizeof line, in))
    {
        size_t n = strlen(line);
        if (!any)
            crlf = n >= 2 && line[n - 2] == '\r', any = 1;
        int k = -1;
        for (int i = 0; i < 6; ++i)
            if (line[0] == '"' && !strncmp(line + 1, REG_NAMES[i], 4) && line[5] == '"')
                k = i;
        if (k < 0)
            fputs(line, out);
        else if (!written[k])
        {
            fprintf(out, "\"%s\"=dword:%08lx%s", REG_NAMES[k], (unsigned long)v[k], crlf ? "\r\n" : "\n");
            written[k] = 1;
        }
    }
    if (in)
        fclose(in);
    else
        fprintf(out, "REGEDIT4\r\n\r\n[HKEY_LOCAL_MACHINE\\SOFTWARE\\PlayOnlineUS\\SquareEnix\\FinalFantasyXI]\r\n");
    for (int i = 0; i < 6; ++i)
        if (!written[i])
            fprintf(out, "\"%s\"=dword:%08lx%s", REG_NAMES[i], (unsigned long)v[i], crlf ? "\r\n" : "\n");
    if (fclose(out))
        return 0;
    return replace_file(DATA_REG, tmp);
}

/* --- from the files, and back -------------------------------------------------------------------------- */
static int nearest(const int* values, int n, long v)
{
    int best = 0;
    for (int i = 1; i < n; ++i)
        if (labs(values[i] - v) < labs(values[best] - v))
            best = i;
    return best;
}

static void load(void)
{
    for (int i = 0; i < NRES; ++i)
        g_res_names[i] = RES[i].name;
    args_read();
    long w = reg_value("0001", 1920), h = reg_value("0002", 1080);
    long mw = reg_value("0037", w / 2);
    const char* fit = args_env("FFXI_SWITCH_FIT");
    int r = 0; /* the screen's, unless run.args turns that off */
    if (fit && *fit == '0')
        for (int i = 1; i < NRES; ++i)
            if (RES[i].w == w && RES[i].h == h)
                r = i;
    g_opt[O_RES].cur = r;
    if (!r)
        w = 1920; /* the interface's size is the docked screen's over the scale */
    float scale = mw > 0 ? (float)w / (float)mw : 2.0f;
    int u = 0;
    for (int i = 1; i < 3; ++i)
        if (fabsf(UI_SCALE[i] - scale) < fabsf(UI_SCALE[u] - scale))
            u = i;
    g_opt[O_UI].cur = u;
    const char* bg = args_env("FFXI_SWITCH_BG");
    g_opt[O_BG].cur = bg && atoi(bg) > 0 ? 1 + nearest(BG + 1, 3, atol(bg)) : 0;

    const char* fps = args_value("--fps-divisor");
    const char* adapt = args_env("FFXI_FPS_ADAPT");
    g_opt[O_FPS].cur = !fps || !strcmp(fps, "0") ? (adapt && atoi(adapt) > 0 ? 1 : 0) : !strcmp(fps, "1") ? 2 : 3;
    const char* layout = args_value("--switch-layout");
    g_opt[O_LAYOUT].cur = layout && !strcmp(layout, "xbox");
    const char* cpu = args_env("FFXI_SWITCH_CPU");
    const char* boost = args_env("FFXI_SWITCH_LOADBOOST");
    g_opt[O_CPU].cur = cpu && atoi(cpu) > 0 ? 2 : boost && !strcmp(boost, "0") ? 1 : 0;
    const char* relogin = args_env("FFXI_SWITCH_RELOGIN");
    g_opt[O_RELOGIN].cur = relogin && !strcmp(relogin, "0");
    const char* crt = args_env("FFXI_CRT");
    g_opt[O_CRT].cur = crt ? (atoi(crt) >= 2 ? 2 : atoi(crt) == 1) : 0;
    const char* curve = args_env("FFXI_CRT_CURVE");
    g_opt[O_CURVE].cur = curve && atoi(curve) > 0;
}

static int save(void)
{
    const Res* r = &RES[g_opt[O_RES].cur];
    long rw = r->w ? r->w : 1920, rh = r->h ? r->h : 1080; /* the screen's: the docked size in the file */
    float s = UI_SCALE[g_opt[O_UI].cur];
    long mw = (long)(rw / s + 0.5f) & ~1L, mh = (long)(rh / s + 0.5f) & ~1L;
    /* the game lays its windows out in at least 512x448 */
    if (mw < 512 || mh < 448)
        mw = rw, mh = rh;
    int b = BG[g_opt[O_BG].cur];
    long bgv = b ? b : reg_value("0003", 2048); /* the screen's: the file's own kept (Config's, unused) */
    const long v[6] = { rw, rh, bgv, b ? b : reg_value("0004", bgv), mw, mh };
    int ok = reg_write(v);

    args_read(); /* as it is now: lines added by hand meanwhile are kept */
    char bgs[16];
    snprintf(bgs, sizeof bgs, "%d", b);
    args_set_env("FFXI_SWITCH_FIT", r->w ? "0" : NULL);
    args_set_env("FFXI_SWITCH_BG", b ? bgs : NULL);
    args_set("--fps-divisor", FPS_VALUES[g_opt[O_FPS].cur]);
    args_set_env("FFXI_FPS_ADAPT", g_opt[O_FPS].cur == 1 ? "1" : NULL);
    args_set("--switch-layout", g_opt[O_LAYOUT].cur ? "xbox" : "nintendo");
    args_set_env("FFXI_SWITCH_LOADBOOST", g_opt[O_CPU].cur == 1 ? "0" : NULL);
    args_set_env("FFXI_SWITCH_CPU", g_opt[O_CPU].cur == 2 ? "1785" : NULL);
    args_set_env("FFXI_SWITCH_RELOGIN", g_opt[O_RELOGIN].cur ? "0" : NULL);
    args_set_env("FFXI_CRT", g_opt[O_CRT].cur == 2 ? "2" : g_opt[O_CRT].cur == 1 ? "1" : NULL);
    args_set_env("FFXI_CRT_CURVE", g_opt[O_CURVE].cur ? "1" : NULL);
    return args_write() && ok;
}

/* --- the screen ----------------------------------------------------------------------------------------- */
static const char* const ACTIONS[] = { "Save and start the game", "Save and exit", "Reset to defaults",
    "Exit without saving" };
#define NACTIONS (int)(sizeof ACTIONS / sizeof ACTIONS[0])

static void draw(int row, const char* status)
{
    printf("\x1b[2J\x1b[1;1H");
    printf("\x1b[36;1m  XI on Anything: settings\x1b[0m\n");
    printf("  Changes apply the next time the game starts.\n\n");
    for (int i = 0; i < NOPTS; ++i)
    {
        const Opt* o = &g_opt[i];
        if (i == row)
            printf("\x1b[33;1m> %-26s < %s >\x1b[0m\n", o->label, o->names[o->cur]);
        else
            printf("  %-26s   %s\n", o->label, o->names[o->cur]);
    }
    printf("\n");
    for (int i = 0; i < NACTIONS; ++i)
        printf(row == NOPTS + i ? "\x1b[33;1m> %s\x1b[0m\n" : "  %s\n", ACTIONS[i]);
    printf("\n");
    if (row < NOPTS)
    {
        /* the help, wrapped at the screen's 78 columns */
        const char* t = g_opt[row].help;
        while (*t)
        {
            int n = (int)strlen(t);
            if (n > 76)
            {
                n = 76;
                while (n > 0 && t[n] != ' ')
                    --n;
                if (!n)
                    n = 76;
            }
            printf("\x1b[37m  %.*s\x1b[0m\n", n, t);
            t += n;
            while (*t == ' ')
                ++t;
        }
    }
    printf("\n\x1b[32m  %s\x1b[0m\n", status ? status : "");
    printf("\x1b[44;1H  D-pad: move and change   A: change / pick   +: save and start   B: exit");
    consoleUpdate(NULL);
}

static int start_game(void)
{
    struct stat st;
    if (stat(GAME_NRO, &st) != 0 || !envHasNextLoad())
        return 0;
    return R_SUCCEEDED(envSetNextLoad(GAME_NRO, "\"" GAME_NRO "\""));
}

int main(int argc, char** argv)
{
    (void)argc, (void)argv;
    consoleInit(NULL);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);

    struct stat st;
    const char* status = stat(APP_DIR, &st) == 0 ? NULL : "No " APP_DIR " on the card: set the game up first (setup-switch).";
    load();
    int row = 0, dirty = 0;
    draw(row, status);
    while (appletMainLoop())
    {
        padUpdate(&pad);
        u64 k = padGetButtonsDown(&pad);
        if (!k)
        {
            svcSleepThread(16000000ll);
            continue;
        }
        int act = -1;
        if (k & HidNpadButton_AnyUp)
            row = (row + NROWS - 1) % NROWS;
        else if (k & HidNpadButton_AnyDown)
            row = (row + 1) % NROWS;
        else if (row < NOPTS && (k & (HidNpadButton_AnyLeft | HidNpadButton_AnyRight | HidNpadButton_A)))
        {
            Opt* o = &g_opt[row];
            o->cur = (o->cur + ((k & HidNpadButton_AnyLeft) ? o->n - 1 : 1)) % o->n;
            dirty = 1;
        }
        else if (row >= NOPTS && (k & HidNpadButton_A))
            act = row;
        else if (k & HidNpadButton_Plus)
            act = A_START;
        else if (k & HidNpadButton_B)
            act = A_QUIT;
        status = dirty ? "Not saved yet." : status;
        if (act == A_RESET)
        {
            /* every setting's default is its first choice; saved only when saved */
            for (int i = 0; i < NOPTS; ++i)
                g_opt[i].cur = 0;
            dirty = 1;
            status = "Back to the defaults. Not saved yet: Save to keep them.";
        }
        if (act == A_START || act == A_SAVE)
        {
            if (!save())
            {
                status = "Could not write the settings on the card.";
                draw(row, status);
                continue;
            }
            if (act == A_START && !start_game())
            {
                status = "Saved. Start XI on Anything from the homebrew menu (hold R over a game).";
                dirty = 0;
                draw(row, status);
                continue;
            }
            break;
        }
        if (act == A_QUIT)
            break;
        draw(row, status);
    }
    consoleExit(NULL);
    return 0;
}
