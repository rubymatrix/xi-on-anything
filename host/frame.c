/* FFXiMain past 60 fps.
 *
 * The game steps its world by a frame delta in sixtieths of a second. Its frame pacer (once a
 * frame, "frame_pace") measures the frame, averages the last four, truncates the average to whole
 * sixtieths and keeps it between the frame-rate divisor and 20, at +0x28 of the game object; two
 * getters ("frame_dt", "frame_dt2") hand it out, never below 1.0. At a divisor of 1 or 2 that is
 * exact: frames are 1 or 2 sixtieths long. At 0 (no wait) every frame counts as a whole sixtieth
 * however short it was, so the world runs as fast as the frames come: 120 fps plays at double speed.
 *
 * While the player's frame rate is Uncapped, the getters' callers get the real delta instead
 * (wall-clock time since the last frame, in sixtieths, at most 20 as the game keeps it), except
 * the callers tools/frame_sites.py found truncating it into integer state (a countdown
 * t = (int)(t - dt), a step count n = (int)dt): a fraction there loses or gains a whole step every
 * frame, so they get the frame's whole sixtieths, the remainder carried to the next frame. Over
 * time both add up to the wall clock, as at 60 fps. The frame counter's getter ("frame_count",
 * which a few callers turn into time: an angle 16 degrees a frame) counts those sixtieths too.
 *
 * The pacer still stores the delta for the code that reads +0x28 itself (its 60 / delta, the
 * frame rate it keeps); the hook puts the real one there. The player's cap (Frame Rate 120, 144), or
 * FFXI_FPS_LIMIT=n over it, waits out each frame to an even pace while uncapped (vsync, on unless
 * FFXI_VSYNC=0, already holds it to the display).
 * FFXI_FRAMELOG=1 logs the frame rate, the mean delta and the sixtieths handed out each second. */
#include "frame.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "addons/game.h"
#include "build.h"
#include "gthread.h"
#include "plat.h"
#include "runtime.h"

#if defined(FFXI_HOOK_FRAME_PACE) && defined(FFXI_WRAP_FRAME_DT) && defined(FFXI_WRAP_FRAME_DT2) && \
    defined(FFXI_WRAP_FRAME_COUNT) && defined(FFXI_WRAP_FRAME_YIELD) && defined(FFXI_FRAME_INT_SITE_COUNT) && \
    defined(FFXI_FRAME_WAIT_SITE_COUNT) && defined(FFXI_HOOK_FRAME_EASE) && \
    defined(FFXI_WRAP_FRAME_NET) && defined(FFXI_FRAME_CAM_ONCE) && defined(FFXI_HOOK_FRAME_CAM_TICK) && \
    defined(FFXI_HOOK_FRAME_CAM_SWING) && defined(FFXI_HOOK_FRAME_CAM_WAIT) && defined(FFXI_HOOK_FRAME_LOOK1) && \
    defined(FFXI_HOOK_FRAME_LOOK2) && defined(FFXI_HOOK_FRAME_CAM1) && defined(FFXI_HOOK_FRAME_CAM2) && defined(FFXI_HOOK_FRAME_CAM3) && defined(FFXI_HOOK_FRAME_CAM4)
#define HAVE_FRAME 1
extern GuestFn rt_wrap_frame_net;
extern const GuestFn rt_orig_frame_net;
extern GuestFn rt_hook_frame_cam_tick, rt_hook_frame_cam_swing, rt_hook_frame_cam_wait, rt_hook_frame_look1, rt_hook_frame_look2;
/* the third-person camera's step count and its two eases' toward the player */
static const uint32_t CAM_ONCE[] = { FFXI_FRAME_CAM_ONCE };
extern GuestFn rt_hook_frame_cam1, rt_hook_frame_cam2, rt_hook_frame_cam3, rt_hook_frame_cam4;
extern GuestFn rt_hook_frame_pace, rt_hook_frame_ease, rt_wrap_frame_dt, rt_wrap_frame_dt2, rt_wrap_frame_count, rt_wrap_frame_yield;
extern const GuestFn rt_orig_frame_dt, rt_orig_frame_dt2, rt_orig_frame_count, rt_orig_frame_yield;
static const uint32_t INT_SITES[] = { FFXI_FRAME_INT_SITES };   /* ascending */
static const uint32_t WAIT_SITES[] = { FFXI_FRAME_WAIT_SITES }; /* the lobby's counted waits */
#endif

static int g_on;           /* this frame: the player's uncapped frame rate, the real delta handed out */
static int g_menus30;      /* this frame: out of the world at the player's 60 fps or uncapped, kept to the menus' 30 fps pace */
static double g_dt = 1.0;  /* this frame's delta, sixtieths */
static double g_carry;     /* the sixtieths not yet handed out whole */
static double g_dt_sum;    /* 0 to start the steady delta again */
static uint32_t g_whole;   /* this frame's whole sixtieths */
static uint32_t g_ticks = 1; /* every whole sixtieth so far: the frame counter's stand-in */
static uint64_t g_last;    /* the last pace's clock, ns; 0 to start again */
static uint32_t g_limit;   /* this frame's cap: the player's, or FFXI_FPS_LIMIT */
static uint32_t g_env_limit; /* FFXI_FPS_LIMIT */
static uint64_t g_next;    /* the limiter's next frame, ns */
static int g_log;
static uint32_t g_log_frames, g_log_whole;
static double g_log_dt;
static uint64_t g_log_since;
/* bisecting (FFXI_FRAME_MODE, FFXI_FRAME_FLIP, FFXI_FRAMELOG=2): every caller a float, every caller
 * whole sixtieths, or the game's own getter; callers whose kind is flipped; the callers seen each second */
enum { MODE_SPLIT, MODE_FLOAT, MODE_WHOLE, MODE_ORIG };
static int g_mode;
static uint32_t g_flip[64];
static unsigned g_nflip;
#define SITE_SLOTS 512
static struct { uint32_t ret, n; } g_seen[SITE_SLOTS];

#if HAVE_FRAME
static int int_site(uint32_t ret)
{
    unsigned lo = 0, hi = FFXI_FRAME_INT_SITE_COUNT;
    while (lo < hi)
    {
        unsigned mid = (lo + hi) / 2;
        if (INT_SITES[mid] == ret)
            return 1;
        if (INT_SITES[mid] < ret)
            lo = mid + 1;
        else
            hi = mid;
    }
    return 0;
}

static int flipped(uint32_t ret)
{
    for (unsigned i = 0; i < g_nflip; ++i)
        if (g_flip[i] == ret)
            return 1;
    return 0;
}

static void seen(uint32_t ret)
{
    unsigned h = (ret * 2654435761u) >> 23;
    for (unsigned i = 0; i < SITE_SLOTS; ++i, h = (h + 1) & (SITE_SLOTS - 1))
        if (g_seen[h].ret == ret || !g_seen[h].ret)
        {
            g_seen[h].ret = ret, ++g_seen[h].n;
            return;
        }
}

static void seen_report(void)
{
    char line[4096];
    int n = 0;
    for (unsigned i = 0; i < SITE_SLOTS; ++i)
        if (g_seen[i].ret && n < (int)sizeof line - 32)
            n += snprintf(line + n, sizeof line - n, " %08x%s:%u", g_seen[i].ret, int_site(g_seen[i].ret) ? "i" : "f",
                g_seen[i].n);
    rt_log("[recomp] frame sites:%s\n", n ? line : " none");
    memset(g_seen, 0, sizeof g_seen);
}

/* the pacer, the game object in esi, after it stored its delta (+0x28) and the same at most 20 (+0x2c) */
static void frame_pace(Guest* g)
{
    if (!g_on)
        return;
    uint64_t now = rt_monotonic_ns();
    double dt = g_last ? (double)(now - g_last) * 60e-9 : 1.0;
    g_last = now;
    if (dt > 20.0)
        dt = 20.0; /* a stall (loading, a dragged window) steps no further than the game's own cap */
    if (dt < 1e-3)
        dt = 1e-3;
    /* A steady delta: the median of the last 15 frames' lengths, nudged by a twentieth of how far the
     * world's time has drifted from the wall clock's. A frame's own length jitters with when the display
     * takes it (the game waits for a drawable), while the display shows them at an even pace; handing
     * out the raw length would move things unevenly from one shown frame to the next. Over time the
     * world's time still is the wall clock's. */
    static double ring[15], world, wall;
    static unsigned at, n;
    if (!g_dt_sum)
        n = 0, at = 0, world = wall = 0, g_dt_sum = 1; /* (re)starting */
    ring[at] = dt, at = (at + 1) % 15;
    if (n < 15)
        ++n;
    double sorted[15];
    memcpy(sorted, ring, sizeof sorted);
    for (unsigned i = 1; i < n; ++i)
        for (unsigned j = i; j && sorted[j - 1] > sorted[j]; --j)
        {
            double t = sorted[j];
            sorted[j] = sorted[j - 1], sorted[j - 1] = t;
        }
    double median = sorted[n / 2];
    wall += dt;
    /* a dropped frame (half again the usual length) moves the world the whole of it at once - one
     * hitch - instead of the usual step now and the rest caught up over the frames after (a slow-down,
     * then a rush) */
    if (dt <= median * 1.5)
    {
        double fix = (wall - world) * 0.05;
        if (fix > median * 0.25)
            fix = median * 0.25;
        if (fix < -median * 0.25)
            fix = -median * 0.25;
        dt = median + fix;
        if (wall - world > 20.0 || world - wall > 20.0)
            world = wall; /* a stall: start again from the wall clock */
    }
    world += dt;
    g_dt = dt;
    g_carry += dt;
    g_whole = (uint32_t)g_carry;
    g_carry -= g_whole;
    g_ticks += g_whole;
    if (!g_ticks)
        g_ticks = 1; /* as the game's counter: never 0 */
    float f = (float)dt;
    if (g_mode != MODE_ORIG)
        wrf32(g->esi + 0x28, f), wrf32(g->esi + 0x2c, f);
    if (g_log)
    {
        ++g_log_frames, g_log_dt += dt, g_log_whole += g_whole;
        if (!g_log_since)
            g_log_since = now;
        if (now - g_log_since >= 1000000000ull)
        {
            double s = (double)(now - g_log_since) * 1e-9;
            rt_log("[recomp] frame: %.1f fps, delta %.3f (%.1f sixtieths/s), %u whole (%.1f/s)\n", g_log_frames / s,
                g_log_dt / g_log_frames, g_log_dt / s, g_log_whole, g_log_whole / s);
            g_log_frames = g_log_whole = 0, g_log_dt = 0, g_log_since = now;
            if (g_log >= 2)
                seen_report();
        }
    }
}

/* a delta getter: fld [game+0x28], at least 1.0 (ecx = the game object, eax the FPU status) */
static void frame_dt_value(Guest* g, const GuestFn orig)
{
    if (!g_on || g_mode == MODE_ORIG)
    {
        orig(g);
        return;
    }
    uint32_t ret = rd32(g->esp);
    if (g_log >= 2)
        seen(ret);
    /* the third-person camera steps once every frame (its step made the frame's length: cam_ease and the
     * hooks after it), so it moves with the character it follows instead of a whole sixtieth at a time */
    if (g_mode == MODE_SPLIT && (ret == CAM_ONCE[0] || ret == CAM_ONCE[1] || ret == CAM_ONCE[2] || ret == CAM_ONCE[3]))
    {
        fpush(g, 1.0);
        rt_return(g, g->eax, 0);
        return;
    }
    int whole = g_mode == MODE_WHOLE || (g_mode == MODE_SPLIT && int_site(ret));
    if (g_nflip && flipped(ret))
        whole = !whole;
    fpush(g, whole ? (double)g_whole : g_dt);
    rt_return(g, g->eax, 0);
}

static void frame_dt(Guest* g) { frame_dt_value(g, rt_orig_frame_dt); }
static void frame_dt2(Guest* g) { frame_dt_value(g, rt_orig_frame_dt2); }

static void frame_count(Guest* g)
{
    if (!g_on || g_mode == MODE_ORIG)
    {
        rt_orig_frame_count(g);
        return;
    }
    rt_return(g, g_ticks, 0);
}

/* The lobby runs as a coroutine on its own thread, waiting a frame at a time (cdecl, one argument:
 * a flag to wait for, or 0 for one frame; -1 when the lobby is being left). Some of its loops count
 * those frames (the scene's fades and transitions, the network bar held up for so many), timed for
 * the 30 fps the game shipped at: at 60 fps and uncapped, one wait there lasts until the next
 * thirtieth of the wall clock, so they take as long as they were made to; the deadline carries
 * over, so the steps average 30 a second. Its other waits poll the network's state for a value
 * that can last a single frame, and stay a frame long. */
static uint64_t g_yield_due;

static void frame_yield(Guest* g)
{
    uint32_t ret = rd32(g->esp);
    int counted = 0;
    for (unsigned i = 0; i < FFXI_FRAME_WAIT_SITE_COUNT; ++i)
        counted |= WAIT_SITES[i] == ret;
    /* a wait on a flag (argument nonzero) ends when the flag is set: as the game does it */
    if (!g_menus30 || g_mode == MODE_ORIG || rt_arg(g, 0) || !counted)
    {
        rt_orig_frame_yield(g);
        return;
    }
    const uint64_t tick = 1000000000ull / 30;
    uint64_t now = rt_monotonic_ns();
    g_yield_due = g_yield_due && now < g_yield_due + tick ? g_yield_due + tick : now + tick;
    uint32_t esp = g->esp;
    for (int frames = 0;; ++frames)
    {
        rt_orig_frame_yield(g); /* returns for the caller: esp past the return address */
        if ((int32_t)g->eax < 0 || !g_menus30 || rt_monotonic_ns() >= g_yield_due || frames > 240)
            return;
        g->esp = esp; /* the caller's return address and argument are still there: wait again */
    }
}

/* The zone connection's tick runs once a frame: it handles what has arrived and sends what is due
 * (its sends keep their own clock). Uncapped, it runs 30 times a second, as at the 30 fps the
 * game shipped at; the frames between return as a tick with nothing to do does (cdecl, al = 1). */
static uint64_t g_net_due;
static uint32_t g_net_ms; /* the connection clock's milliseconds of the frames skipped */

static void frame_net(Guest* g)
{
    if (!g_on || g_mode == MODE_ORIG)
    {
        rt_orig_frame_net(g);
        return;
    }
    /* the connection clock ([0x10492e10]: +8 the milliseconds since its last frame) is moved on every
     * frame; its timers (when to send) add +8 up at each tick, so a tick is given the skipped frames' too */
    uint32_t clock = rd32(0x10492e10);
    const uint64_t tick = 1000000000ull / 30;
    uint64_t now = rt_monotonic_ns();
    if (g_net_due && now < g_net_due)
    {
        if (clock)
            g_net_ms += rd32(clock + 8);
        rt_return(g, (g->eax & ~0xffu) | 1u, 0);
        return;
    }
    g_net_due = g_net_due && now < g_net_due + tick ? g_net_due + tick : now + tick;
    if (clock)
        wr32(clock + 8, rd32(clock + 8) + g_net_ms);
    g_net_ms = 0;
    rt_orig_frame_net(g);
}

/* The scene camera (the title's, the character select's, a cutscene's) eases toward its next pose
 * by a fraction k of the way each frame, its time apart: past the frame rate it was made for, the
 * ease ends sooner. The fraction is made one for the frame's length - 1 - (1 - k)^(frames), in
 * frames of 30 a second out of the world (the pace it was made for) and of 60 in the world (the
 * pace players know from 60 fps). k is on the x87 stack and at [esp+0xc]. */
/* how many frames of the pace being kept this frame is: of 30 a second out of the world, of 60 in it;
 * 0 when the game's own pacing is in effect (30 fps, or 60 in the world) */
static double ease_frames(void)
{
    if (!g_on && !g_menus30)
        return 0.0;
    return (g_on ? g_dt : 1.0) * (g_menus30 ? 0.5 : 1.0);
}

/* a fraction k of the way a frame, made one for this frame's length */
static double ease_step(double k, double frames) { return 1.0 - pow(1.0 - k, frames); }

static void frame_ease(Guest* g)
{
    double frames = ease_frames(), k = ST(0);
    if (!frames || !(k > 0.0 && k < 1.0))
        return;
    double e = ease_step(k, frames);
    ST(0) = e;
    wrf32(g->esp + 0xc, (float)e);
}

/* The third-person camera keeps its distance from what it looks at between 3 and 6 by a fraction of
 * the way each frame (back by half past 100, by 0.012 past 6, out by 0.125 under 3 - which is the
 * character select's opening move). Hooked before each fmul by k, with the step's other factor on the
 * x87 stack: scaled so the product is the step for this frame's length. */
static void cam_ease(Guest* g, double k)
{
    double frames = ease_frames();
    if (frames)
        ST(0) *= ease_step(k, frames) / k;
}

static void frame_cam1(Guest* g) { cam_ease(g, 0.5); }
static void frame_cam2(Guest* g) { cam_ease(g, 0.012); }
static void frame_cam3(Guest* g) { cam_ease(g, 0.125); }
static void frame_cam4(Guest* g) { cam_ease(g, 0.125); }

/* the step's counter, down by 1 a step: by the frame's delta instead (before fsub [1.0], st0 the counter) */
static void frame_cam_tick(Guest* g)
{
    if (g_on && g_mode == MODE_SPLIT)
        ST(0) += 1.0 - g_dt;
}

/* the step's wait after the player moves, down 1 a step (eax, about to be stored): only on frames a whole
 * sixtieth ends, so it waits as long as at 60 fps */
static void frame_cam_wait(Guest* g)
{
    if (g_on && g_mode == MODE_SPLIT && !g_whole)
        g->eax += 1;
}

/* The camera's look-at follows the player by a quarter, and in another state an eighth, of the way a
 * step; stepped once a frame, the fraction (the scale's argument, at [esp+4] at the call) is made
 * the one for the frame's length, so it keeps up with the player every frame */
static void look_ease(Guest* g, double k)
{
    double frames = ease_frames();
    if (frames && g_mode == MODE_SPLIT)
        wrf32(g->esp + 4, (float)ease_step(k, frames));
}

static void frame_look1(Guest* g) { look_ease(g, 0.25); }
static void frame_look2(Guest* g) { look_ease(g, 0.125); }

/* the step's swing behind the player, a fixed amount a step: times the frame's delta (before its last fmul) */
static void frame_cam_swing(Guest* g)
{
    if (g_on && g_mode == MODE_SPLIT)
        ST(0) *= g_dt;
}
#endif

int frame_supported(void)
{
#if HAVE_FRAME
    return 1;
#else
    return 0;
#endif
}

void frame_setup(void)
{
    const char* s = getenv("FFXI_FPS_LIMIT");
    long n = s ? strtol(s, NULL, 10) : 0;
    g_env_limit = n > 0 && n <= 1000 ? (uint32_t)n : 0;
    s = getenv("FFXI_FRAMELOG");
    g_log = s ? atoi(s) : 0;
    s = getenv("FFXI_FRAME_MODE");
    g_mode = !s ? MODE_SPLIT : !strcmp(s, "float") ? MODE_FLOAT : !strcmp(s, "whole") ? MODE_WHOLE : !strcmp(s, "orig") ? MODE_ORIG : MODE_SPLIT;
    for (s = getenv("FFXI_FRAME_FLIP"); s && *s && g_nflip < 64;)
    {
        char* end;
        unsigned long a = strtoul(s, &end, 16);
        if (end == s)
            break;
        g_flip[g_nflip++] = (uint32_t)a;
        s = *end ? end + 1 : end;
    }
#if HAVE_FRAME
    rt_hook_frame_pace = frame_pace;
    rt_hook_frame_ease = frame_ease;
    rt_hook_frame_cam1 = frame_cam1, rt_hook_frame_cam2 = frame_cam2;
    rt_hook_frame_cam3 = frame_cam3, rt_hook_frame_cam4 = frame_cam4;
    rt_hook_frame_cam_tick = frame_cam_tick, rt_hook_frame_cam_swing = frame_cam_swing;
    rt_hook_frame_cam_wait = frame_cam_wait;
    rt_hook_frame_look1 = frame_look1, rt_hook_frame_look2 = frame_look2;
    rt_wrap_frame_dt = frame_dt;
    rt_wrap_frame_dt2 = frame_dt2;
    rt_wrap_frame_count = frame_count;
    rt_wrap_frame_yield = frame_yield;
    rt_wrap_frame_net = frame_net;
#else
    rt_log("[recomp] frame: build %s has no frame delta sites (tools/frame_sites.py); uncapped runs as the game does\n",
        FFXI_BUILD);
#endif
}


/* FFXI_MOTIONLOG=<file> (debugging): a line a frame - the time, the delta, the player's position and the
 * camera's eye - for measuring how evenly things move from frame to frame */
static void motion_log(void)
{
    static FILE* f;
    static int init;
    if (!init)
    {
        init = 1;
        const char* p = getenv("FFXI_MOTIONLOG");
        if (p)
            f = fopen(p, "w");
    }
    if (!f)
        return;
    int32_t pi = xi_game_player_index();
    uint32_t e = pi >= 0 ? xi_game_entity((uint32_t)pi) : 0, game = rd32(0x104568fc), cam = game ? rd32(game + 0x50) : 0;
    float p[3] = { 0 }, c[6] = { 0 };
    if (e)
        xi_game_rd(e + 4, p, sizeof p);
    if (cam)
        xi_game_rd(cam + 0x44, c, sizeof c);
    fprintf(f, "%llu %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %u\n", (unsigned long long)rt_monotonic_ns(), g_on ? g_dt : 0.0,
        p[0], p[1], p[2], c[0], c[1], c[2], c[3], c[4], c[5], g_whole);
    fflush(f);
}

void frame_present(int uncapped, int menus30, uint32_t limit)
{
    motion_log();
    g_menus30 = menus30;
    if ((g_env_limit ? g_env_limit : limit) != g_limit)
        g_limit = g_env_limit ? g_env_limit : limit, g_next = 0;
#if HAVE_FRAME
    if (uncapped != g_on)
    {
        g_on = uncapped;
        g_last = 0, g_carry = 0, g_whole = 0, g_dt = 1.0, g_next = 0, g_dt_sum = 0;
        rt_log("[recomp] frame: %s\n", g_on ? "uncapped, the world at the wall clock's speed" : "the game's own pacing");
    }
#endif
    if (!g_on || !g_limit)
        return;
    uint64_t period = 1000000000ull / g_limit, now = rt_monotonic_ns();
    if (!g_next || now > g_next + period)
        g_next = now; /* late by a whole frame: start again from now, not catch up */
    else if (g_next > now)
    {
        /* the guest lock let go meanwhile, as the game's own Sleep does: its network threads run */
        int held = gt_holds();
        if (held)
            gt_unlock();
        while (g_next > now + 2000000ull) /* sleep to within 2 ms, then spin the rest */
        {
            plat_sleep_ms((uint32_t)((g_next - now) / 1000000ull) - 1);
            now = rt_monotonic_ns();
        }
        while (rt_monotonic_ns() < g_next)
            ;
        if (held)
            gt_lock();
    }
    g_next += period;
}
