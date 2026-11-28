/* drawcost: what both paths share - the scene's data, the config, the log, the timing and the statistics. */
#include "common.h"

#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <switch.h>

const DcMode dc_modes[] = {
    { "game", DC_GAME },     /* the game's frame (common.h) */
    { "full", DC_BIND_ALL }, /* the D3D8-like draw: everything rebound */
    { "none", 0 },           /* draws only, nothing rebound */
    { "ubo", DC_BIND_UBO },
    { "tex", DC_BIND_TEX },
    { "vb", DC_BIND_VB },
    { "prog", DC_BIND_PROG },
};
const int dc_nmodes = sizeof(dc_modes) / sizeof(dc_modes[0]);

DcConfig dc_cfg = {
    .draws = 1500,
    .prog_every = 4,
    .vsync = 0,
    .sync = 1,
    .sweep = 1,
    .sweep_secs = 6,
    .pthread_core = -2,
    .mode = 0,
};

static const char* g_path_name = "?";
static int g_main_core;
DcCounts dc_cnt;

/* --- the game mode's frame -------------------------------------------------------------------------------- */
DcGameDraw dc_game[DC_G_DRAWS];

void dc_game_plan(void)
{
    memset(dc_game, 0, sizeof dc_game);
    for (int i = 0; i < DC_G_DRAWS; ++i)
    {
        DcGameDraw* d = &dc_game[i];
        d->prog = (uint8_t)((i / DC_G_PROG_EVERY) % DC_G_NPROG);
        d->tex0 = (uint8_t)((i / 2) % DC_NTEX);
        d->tex1 = (uint8_t)((i / 8 + 5) % DC_NTEX);
        d->samp0 = (uint8_t)((i / 55) % DC_G_NSAMP);
        d->src = (uint8_t)((i * 2 / 5) % (DC_G_NSTATIC + 1));
        d->kind = i % 80 == 3 ? DC_K_STATIC_IDX : i % 4 == 1 ? DC_K_RING_IDX : DC_K_ARRAYS;
        d->ib = (uint8_t)((i / 80) % DC_NIB);
        d->state = i % 3 == 0 ? (uint8_t)(DC_ST_BLEND + (i / 3) % 4) : DC_ST_NONE;
        d->rt = DC_RT_MAIN;
    }
    /* the visits away from the main target: 25 to the side target, 10 to the probe (cleared), 5 to the 256x256 */
    for (int v = 0; v < DC_G_NVISITS; ++v)
    {
        int at = 12 + v * 27, k = v % 8;
        int rt = k < 5 ? DC_RT_SIDE : k < 7 ? DC_RT_PROBE : DC_RT_256;
        for (int j = 0; j < DC_G_VISIT_LEN && at + j < DC_G_DRAWS; ++j)
            dc_game[at + j].rt = (uint8_t)rt;
        dc_game[at].clear = rt == DC_RT_PROBE;
    }
}

void dc_game_words(int mesh, int indexed, uint32_t* out)
{
    DcVertex v[DC_NVERT];
    dc_mesh(mesh, v);
    int n = indexed ? DC_NVERT : DC_G_ARRAY_VERTS;
    for (int i = 0; i < n; ++i)
        memcpy(out + 4 * i, &v[indexed ? i : dc_indices[i]], sizeof(DcVertex));
}

void dc_game_uniforms(int draw, int base_words, float* block)
{
    DcUniforms u;
    dc_uniforms(draw, &u);
    memcpy(block, u.xform, 32);
    int32_t base[4] = { base_words, 0, 0, 0 };
    memcpy(block + 8, base, 16);
}

/* --- the scene ---------------------------------------------------------------------------------------- */
const uint16_t dc_indices[DC_NIDX] = {
    0, 1, 4, 0, 4, 3, 1, 2, 5, 1, 5, 4, 3, 4, 7, 3, 7, 6, 4, 5, 8, 4, 8, 7,
};

/* a 2x2 grid of quads over [-1,1], counter-clockwise; each buffer a little different in size */
void dc_mesh(int vb, DcVertex out[DC_NVERT])
{
    float s = 0.65f + 0.05f * (float)vb;
    for (int y = 0; y < 3; ++y)
        for (int x = 0; x < 3; ++x)
        {
            DcVertex* v = &out[y * 3 + x];
            v->pos[0] = (-1.0f + (float)x) * s;
            v->pos[1] = (-1.0f + (float)y) * s;
            v->uv[0] = 0.5f * (float)x;
            v->uv[1] = 0.5f * (float)y;
        }
}

/* a checkerboard in a colour of its own */
void dc_texture(int t, uint32_t out[DC_TEX_SIZE * DC_TEX_SIZE])
{
    uint32_t r = 64 + (uint32_t)((t * 53) & 191), g = 64 + (uint32_t)((t * 97) & 191), b = 64 + (uint32_t)((t * 151) & 191);
    for (int y = 0; y < DC_TEX_SIZE; ++y)
        for (int x = 0; x < DC_TEX_SIZE; ++x)
        {
            uint32_t k = ((x >> 2) ^ (y >> 2)) & 1 ? 255 : 160;
            out[y * DC_TEX_SIZE + x] = (r * k / 255) | (g * k / 255) << 8 | (b * k / 255) << 16 | 0xFFu << 24;
        }
}

/* draw i's place on an 80x60 grid over the screen, and a colour */
void dc_uniforms(int draw, DcUniforms* u)
{
    memset(u, 0, sizeof(*u));
    int col = draw % 80, row = (draw / 80) % 60;
    u->xform[0] = -1.0f + ((float)col + 0.5f) * (2.0f / 80.0f);
    u->xform[1] = 1.0f - ((float)row + 0.5f) * (2.0f / 60.0f);
    u->xform[2] = 0.8f / 80.0f;
    u->xform[3] = 0.8f / 60.0f;
    u->color[0] = 0.5f + 0.5f * (float)((draw * 7) % 11) / 10.0f;
    u->color[1] = 0.5f + 0.5f * (float)((draw * 3) % 13) / 12.0f;
    u->color[2] = 0.5f + 0.5f * (float)((draw * 5) % 17) / 16.0f;
    u->color[3] = 1.0f;
}

/* --- the log ------------------------------------------------------------------------------------------ */
static FILE* g_log;

void dc_log(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    if (g_log)
    {
        vfprintf(g_log, fmt, ap);
        fputc('\n', g_log);
        fflush(g_log);
    }
    va_end(ap);
}

uint64_t dc_now_ns(void) { return armTicksToNs(armGetSystemTick()); }

/* --- the threads pthread_create makes (Mesa's: glthread's worker, its shader queues) --------------------------
 * Linked with --wrap=__syscall_thread_create, as the game is (platforms/switch/switch_main.c). libnx's
 * struct __pthread_t begins with its Thread, whose first member is the handle. Nothing here writes stdio. */
#define DC_MAX_THREADS 16
struct __pthread_t;
int __real___syscall_thread_create(struct __pthread_t** thread, void* (*func)(void*), void* arg, void* stack, size_t sz);
static struct
{
    uint32_t n;
    Handle h[DC_MAX_THREADS];
    int32_t core[DC_MAX_THREADS], prio[DC_MAX_THREADS], moved[DC_MAX_THREADS], by_core[DC_MAX_THREADS];
} g_pt;
static uint32_t g_pt_said;

int __wrap___syscall_thread_create(struct __pthread_t** thread, void* (*func)(void*), void* arg, void* stack, size_t sz)
{
    int r = __real___syscall_thread_create(thread, func, arg, stack, sz);
    if (r || !thread || !*thread)
        return r;
    Handle h = *(const Handle*)*thread;
    s32 core = -1, prio = -1;
    u64 mask = 0;
    svcGetThreadCoreMask(&core, &mask, h);
    svcGetThreadPriority(&prio, h);
    int moved = -1;
    int want = dc_cfg.pthread_core;
    if (want == -2)
        want = (g_main_core + 1) % 3;
    if (want >= 0 && want <= 2 && R_SUCCEEDED(svcSetThreadCoreMask(h, want, 1ull << want)))
        moved = want;
    uint32_t i = __atomic_fetch_add(&g_pt.n, 1, __ATOMIC_RELAXED);
    if (i < DC_MAX_THREADS)
    {
        g_pt.h[i] = h;
        g_pt.core[i] = core, g_pt.prio[i] = prio, g_pt.moved[i] = moved;
        g_pt.by_core[i] = (int32_t)svcGetCurrentProcessorNumber();
    }
    return r;
}

int dc_nthreads(void)
{
    uint32_t n = __atomic_load_n(&g_pt.n, __ATOMIC_ACQUIRE);
    return (int)(n < DC_MAX_THREADS ? n : DC_MAX_THREADS);
}

static uint64_t thread_cpu_ns(Handle h)
{
    u64 ticks = 0;
    if (R_FAILED(svcGetInfo(&ticks, InfoType_ThreadTickCount, h, UINT64_MAX)))
        return 0;
    return armTicksToNs(ticks);
}

uint64_t dc_main_cpu_ns(void) { return thread_cpu_ns(CUR_THREAD_HANDLE); }
uint64_t dc_thread_cpu_ns(int i) { return i < dc_nthreads() ? thread_cpu_ns(g_pt.h[i]) : 0; }

void dc_log_threads(void)
{
    int n = dc_nthreads();
    for (; (int)g_pt_said < n; ++g_pt_said)
    {
        int i = (int)g_pt_said;
        char moved[32] = "";
        if (g_pt.moved[i] >= 0)
            snprintf(moved, sizeof moved, ", moved to core %d", (int)g_pt.moved[i]);
        dc_log("thread thr%d: made by a thread on core %d, on core %d at priority 0x%x%s", i, (int)g_pt.by_core[i],
               (int)g_pt.core[i], (unsigned)g_pt.prio[i], moved);
    }
}

/* --- the config --------------------------------------------------------------------------------------- */
static void config(const char* dir)
{
    char path[600];
    snprintf(path, sizeof path, "%sdrawcost.cfg", dir);
    FILE* f = fopen(path, "r");
    if (!f)
    {
        dc_log("config: %s not found, the defaults used", path);
        return;
    }
    char line[256];
    while (fgets(line, sizeof line, f))
    {
        char* e = line + strlen(line);
        while (e > line && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t'))
            *--e = 0;
        char* s = line;
        while (*s == ' ' || *s == '\t')
            ++s;
        if (!*s || *s == '#')
            continue;
        char* eq = strchr(s, '=');
        if (!eq)
        {
            dc_log("config: ignored \"%s\" (no '=')", s);
            continue;
        }
        *eq = 0;
        const char* v = eq + 1;
        int iv = atoi(v);
        if (!strcmp(s, "draws"))
            dc_cfg.draws = iv;
        else if (!strcmp(s, "prog_every"))
            dc_cfg.prog_every = iv;
        else if (!strcmp(s, "vsync"))
            dc_cfg.vsync = iv;
        else if (!strcmp(s, "sync"))
            dc_cfg.sync = iv;
        else if (!strcmp(s, "sweep"))
            dc_cfg.sweep = iv;
        else if (!strcmp(s, "sweep_secs"))
            dc_cfg.sweep_secs = iv;
        else if (!strcmp(s, "pthread_core"))
            dc_cfg.pthread_core = iv;
        else if (!strcmp(s, "mode"))
        {
            for (int m = 0; m < dc_nmodes; ++m)
                if (!strcmp(v, dc_modes[m].name))
                    dc_cfg.mode = m;
        }
        else
        {
            setenv(s, v, 1);
            dc_log("config: environment %s=%s", s, v);
            continue;
        }
        dc_log("config: %s=%s", s, v);
    }
    fclose(f);
    if (dc_cfg.draws < 1)
        dc_cfg.draws = 1;
    if (dc_cfg.draws > DC_MAX_DRAWS)
        dc_cfg.draws = DC_MAX_DRAWS;
    if (dc_cfg.prog_every < 1)
        dc_cfg.prog_every = 1;
    if (dc_cfg.sweep_secs < 3)
        dc_cfg.sweep_secs = 3;
}

static void clocks(void)
{
    static const struct
    {
        const char* name;
        PcvModuleId id;
    } mods[] = { { "cpu", PcvModuleId_CpuBus }, { "gpu", PcvModuleId_GPU }, { "emc", PcvModuleId_EMC } };
    char s[160] = "clocks:";
    if (R_SUCCEEDED(clkrstInitialize()))
    {
        for (unsigned i = 0; i < sizeof mods / sizeof mods[0]; ++i)
        {
            ClkrstSession ss;
            u32 hz = 0;
            if (R_SUCCEEDED(clkrstOpenSession(&ss, mods[i].id, 3)))
            {
                clkrstGetClockRate(&ss, &hz);
                clkrstCloseSession(&ss);
            }
            size_t l = strlen(s);
            snprintf(s + l, sizeof s - l, " %s %u MHz", mods[i].name, hz / 1000000);
        }
        clkrstExit();
    }
    else
        strcat(s, " not readable (clkrst)");
    dc_log("%s", s);
}

static PadState g_pad;
static char dir[512] = "sdmc:/switch/drawcost/";

const char* dc_dir(void) { return dir; }

void dc_init(int argc, char** argv, const char* path_name, const char* log_name)
{
    g_path_name = path_name;
    g_main_core = (int)svcGetCurrentProcessorNumber();
    dc_game_plan();
    /* the folder the .nro is in (argv[0], sdmc:/switch/drawcost/x.nro), else sdmc:/switch/drawcost/ */
    if (argc > 0 && argv[0] && strchr(argv[0], '/'))
    {
        snprintf(dir, sizeof dir, "%s", argv[0]);
        *(strrchr(dir, '/') + 1) = 0;
        if (dir[0] == '/' && strlen(dir) < sizeof dir - 6) /* hbmenu gives sdmc:/...; a bare /... is the card too */
        {
            memmove(dir + 5, dir, strlen(dir) + 1);
            memcpy(dir, "sdmc:", 5);
        }
    }
    char path[600];
    snprintf(path, sizeof path, "%s%s", dir, log_name);
    g_log = fopen(path, "w");
    if (!g_log)
    {
        mkdir("sdmc:/switch/drawcost", 0777);
        snprintf(dir, sizeof dir, "sdmc:/switch/drawcost/");
        snprintf(path, sizeof path, "%s%s", dir, log_name);
        g_log = fopen(path, "w");
    }
    dc_log("drawcost %s: the CPU cost of a draw (built " __DATE__ " " __TIME__ ")", path_name);
    dc_log("log: %s", path);
    config(dir);
    u32 hv = hosversionGet();
    dc_log("firmware %u.%u.%u, %s, main thread on core %d", HOSVER_MAJOR(hv), HOSVER_MINOR(hv), HOSVER_MICRO(hv),
           appletGetOperationMode() == AppletOperationMode_Console ? "docked" : "handheld", (int)svcGetCurrentProcessorNumber());
    clocks();
    dc_log("settings: draws=%d prog_every=%d vsync=%d sync=%d sweep=%d sweep_secs=%d pthread_core=%d mode=%s", dc_cfg.draws,
           dc_cfg.prog_every, dc_cfg.vsync, dc_cfg.sync, dc_cfg.sweep, dc_cfg.sweep_secs, dc_cfg.pthread_core,
           dc_modes[dc_cfg.mode].name);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&g_pad);
}

void dc_exit(void)
{
    dc_log("exit");
    if (g_log)
        fclose(g_log);
    g_log = NULL;
}

/* --- the statistics ----------------------------------------------------------------------------------- */
typedef struct
{
    int frames, sync_frames;
    uint64_t draws, sync_draws;
    uint64_t first_ns, last_ns; /* the first and last frames' starts */
    uint64_t record_ns, submit_ns, done_ns, present_ns;
    uint64_t cpu0_main, cpu0_thr[DC_MAX_THREADS];
    int nthr;
    uint64_t cnt[sizeof(DcCounts) / sizeof(uint32_t)]; /* dc_cnt's, summed */
} Acc;

static void acc_begin(Acc* a)
{
    memset(a, 0, sizeof(*a));
    a->cpu0_main = dc_main_cpu_ns();
    a->nthr = dc_nthreads();
    for (int i = 0; i < a->nthr; ++i)
        a->cpu0_thr[i] = dc_thread_cpu_ns(i);
}

static void acc_add(Acc* a, const DcFrame* f)
{
    if (!a->frames)
        a->first_ns = f->start_ns;
    a->last_ns = f->start_ns;
    a->frames++;
    a->draws += (uint64_t)f->draws;
    a->record_ns += f->record_ns;
    a->submit_ns += f->submit_ns;
    a->present_ns += f->present_ns;
    if (f->done_ns)
        a->sync_frames++, a->sync_draws += (uint64_t)f->draws, a->done_ns += f->done_ns;
    const uint32_t* c = (const uint32_t*)&dc_cnt;
    for (size_t i = 0; i < sizeof a->cnt / sizeof a->cnt[0]; ++i)
        a->cnt[i] += c[i];
}

static void acc_line(Acc* a, const char* tag, uint64_t t_ns)
{
    if (a->frames < 2 || !a->draws)
        return;
    double period_ms = (double)(a->last_ns - a->first_ns) / 1e6 / (double)(a->frames - 1);
    double d = (double)a->draws;
    char complete[32] = "-";
    if (a->sync_draws)
        snprintf(complete, sizeof complete, "%.2f", (double)a->done_ns / 1e3 / (double)a->sync_draws);
    char cpu[400];
    int l = snprintf(cpu, sizeof cpu, "main=%.2f", (double)(dc_main_cpu_ns() - a->cpu0_main) / 1e3 / d);
    for (int i = 0; i < a->nthr && l < (int)sizeof cpu - 32; ++i)
        l += snprintf(cpu + l, sizeof cpu - (size_t)l, " thr%d=%.2f", i, (double)(dc_thread_cpu_ns(i) - a->cpu0_thr[i]) / 1e3 / d);
    dc_log("%s t=%7.1f %-6s %-4s %-4s N=%-4d frames=%-4d fps=%6.1f frame=%6.2fms | us/draw: record=%.3f submit=%.3f complete=%s"
           " | present=%.2fms | cpu us/draw: %s",
           tag, (double)t_ns / 1e9, g_path_name, dc_modes[dc_cfg.mode].name, dc_cfg.sync ? "sync" : "pipe",
           (int)(a->draws / (uint64_t)a->frames), a->frames, period_ms > 0 ? 1e3 / period_ms : 0.0, period_ms,
           (double)a->record_ns / 1e3 / d, (double)a->submit_ns / 1e3 / d, complete,
           (double)a->present_ns / 1e6 / (double)a->frames, cpu);
    if (a->cnt[0])
    {
        double n = (double)a->frames;
        dc_log("%s   a frame: draws=%.0f programs=%.0f stage_tex=%.0f samplers=%.0f stream_tex=%.0f uniforms=%.0f state=%.0f"
               " index_buf=%.0f targets=%.0f ring=%.2fMB",
               tag, (double)a->cnt[0] / n, (double)a->cnt[1] / n, (double)a->cnt[2] / n, (double)a->cnt[3] / n,
               (double)a->cnt[4] / n, (double)a->cnt[5] / n, (double)a->cnt[6] / n, (double)a->cnt[7] / n, (double)a->cnt[8] / n,
               (double)a->cnt[9] / n / 1e6);
    }
}

/* the sweep: each step sweep_secs, the first 2 s not counted */
static const struct
{
    int mode, sync;
} g_steps[] = { { 0, 1 }, { 1, 1 }, { 2, 1 }, { 3, 1 }, { 4, 1 }, { 5, 1 }, { 6, 1 }, { 0, 0 }, { 1, 0 } };
static int g_step = -1;
static uint64_t g_t0, g_step_t0, g_iv_t0;
static Acc g_iv, g_st;
static int g_st_on;

static void sweep_apply(void)
{
    dc_cfg.mode = g_steps[g_step].mode;
    dc_cfg.sync = g_steps[g_step].sync;
}

static void frame(const DcFrame* f);

void dc_frame(const DcFrame* f)
{
    frame(f);
    memset(&dc_cnt, 0, sizeof dc_cnt);
}

static void frame(const DcFrame* f)
{
    uint64_t now = f->start_ns;
    if (!g_t0)
    {
        g_t0 = g_iv_t0 = g_step_t0 = now;
        acc_begin(&g_iv);
        if (dc_cfg.sweep)
        {
            g_step = 0;
            sweep_apply();
            dc_log("sweep: %d steps of %d s (2 s warm-up each), then %s", (int)(sizeof g_steps / sizeof g_steps[0]),
                   dc_cfg.sweep_secs, "game sync until + is pressed");
        }
        return; /* this frame was drawn before the sweep's first mode */
    }
    dc_log_threads();
    acc_add(&g_iv, f);
    if (g_step >= 0)
    {
        if (!g_st_on && now - g_step_t0 >= 2000000000ull)
            acc_begin(&g_st), g_st_on = 1;
        else if (g_st_on)
            acc_add(&g_st, f);
        if (now - g_step_t0 >= (uint64_t)dc_cfg.sweep_secs * 1000000000ull)
        {
            acc_line(&g_iv, "      ", now - g_t0);
            acc_line(&g_st, "SUMMARY", now - g_t0);
            g_st_on = 0;
            if (++g_step >= (int)(sizeof g_steps / sizeof g_steps[0]))
            {
                g_step = -1;
                dc_cfg.mode = 0, dc_cfg.sync = 1;
                dc_log("sweep done");
            }
            else
                sweep_apply();
            g_step_t0 = g_iv_t0 = now;
            acc_begin(&g_iv);
            return;
        }
    }
    if (now - g_iv_t0 >= 2000000000ull)
    {
        acc_line(&g_iv, "      ", now - g_t0);
        g_iv_t0 = now;
        acc_begin(&g_iv);
    }
}

/* restart the interval when the settings change by hand (and stop the sweep) */
static void changed(const char* what)
{
    if (g_step >= 0)
        dc_log("sweep stopped by the controller");
    g_step = -1, g_st_on = 0;
    g_iv_t0 = dc_now_ns();
    acc_begin(&g_iv);
    dc_log("set: %s -> mode=%s N=%d vsync=%d sync=%d", what, dc_modes[dc_cfg.mode].name, dc_cfg.draws, dc_cfg.vsync, dc_cfg.sync);
}

int dc_input(int* vsync_changed)
{
    *vsync_changed = 0;
    padUpdate(&g_pad);
    u64 k = padGetButtonsDown(&g_pad);
    if (k & HidNpadButton_Plus)
        return 0;
    if (k & HidNpadButton_A)
        dc_cfg.mode = (dc_cfg.mode + 1) % dc_nmodes, changed("A (mode)");
    if (k & HidNpadButton_Up)
        dc_cfg.draws = dc_cfg.draws + 250 > DC_MAX_DRAWS ? DC_MAX_DRAWS : dc_cfg.draws + 250, changed("up (draws)");
    if (k & HidNpadButton_Down)
        dc_cfg.draws = dc_cfg.draws - 250 < 250 ? 250 : dc_cfg.draws - 250, changed("down (draws)");
    if (k & HidNpadButton_X)
        dc_cfg.vsync = !dc_cfg.vsync, *vsync_changed = 1, changed("X (vsync)");
    if (k & HidNpadButton_Y)
        dc_cfg.sync = !dc_cfg.sync, changed("Y (sync)");
    return 1;
}
