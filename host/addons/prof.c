/* The addons' time: every call into an addon's Lua is timed (wall clock and the thread's CPU time),
 * per addon and per kind of call, so a player can see which addon makes the game stutter.
 *
 *   /addon prof               the addons by time spent, per second of play since the last reset
 *   /addon prof <name>        one addon's time per kind of call
 *   /addon prof reset         start counting again
 *   /addon prof slow <ms>     the slow-call log's threshold (0: off; FFXI_ADDON_SLOW sets it at start)
 *
 * The slow-call log (the client's log, never chat) names any single call over the threshold, and any
 * frame that came that late with the addons' share of the wait. Wall time well above CPU time means
 * the call waited (a file write, a lock); a Lua heap that shrank across a slow call means a garbage
 * collection finished inside it.
 *
 * Calls nest (an addon's command runs another addon's handlers): each addon is charged its own time,
 * not the time of the addons it called into. Four clock reads and two heap sizes per call (about
 * 0.3 us on Linux): always on.
 * Windows counts a thread's CPU time in scheduler ticks (about 16 ms), so CPU shares there are rough. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "plat.h"

#include "lua.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <time.h>
#endif

#define CHAT_MODE 207

static const char* const k_slot[XI_PROF_SLOTS] = { "frame", "present", "postrender", "packet_in", "packet_out", "text_in",
    "text_out", "command", "key", "mouse", "load", "other" };

static uint64_t g_since;       /* the last reset */
static uint64_t g_slow_ns = 100000000ull; /* the slow-call log's threshold */
static int g_slow_read;        /* FFXI_ADDON_SLOW looked at */
static uint64_t g_frame_at;    /* the current frame's start */
static uint64_t g_frame_ns;    /* all addons' time in the current frame */
static uint64_t g_frame_cpu;   /* the game thread's CPU time at the frame's start */
static uint64_t g_host_ns;     /* the addon host's frame (addons_frame) in the current frame */
static uint64_t g_logged_at;   /* the slow-call log's last line, and the lines held back since */
static unsigned g_held;
static const XiEvent* g_event; /* the event being raised */

/* the time of the calls running inside the current one, per depth (nested calls) */
static uint64_t g_child_wall[64], g_child_cpu[64];
static int g_depth;

static uint64_t thread_cpu_ns(void)
{
#if defined(_WIN32)
    FILETIME c, e, k, u;
    if (!GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u))
        return 0;
    uint64_t kernel = ((uint64_t)k.dwHighDateTime << 32) | k.dwLowDateTime;
    uint64_t user = ((uint64_t)u.dwHighDateTime << 32) | u.dwLowDateTime;
    return (kernel + user) * 100;
#else
    struct timespec t;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
#endif
}

static int slot_of(const char* where)
{
    for (int i = 0; i < XI_PROF_SLOTS - 1; ++i)
        if (!strncmp(where, k_slot[i], strlen(k_slot[i])))
            return i;
    return XI_PROF_SLOTS - 1;
}

/* A threshold in ms; 0 or less: off. */
static uint64_t ms_to_ns(double ms) { return ms > 0 ? (uint64_t)(ms * 1e6) : 0; }

static uint64_t slow_ns(void)
{
    if (!g_slow_read)
    {
        g_slow_read = 1;
        const char* s = getenv("FFXI_ADDON_SLOW");
        if (s && *s)
            g_slow_ns = ms_to_ns(atof(s));
    }
    return g_slow_ns;
}

/* The local time of day, hh:mm:ss.mmm, to line the log up with what the player saw. */
static void clock_text(char* out, size_t n)
{
#if defined(_WIN32)
    SYSTEMTIME t;
    GetLocalTime(&t);
    snprintf(out, n, "%02d:%02d:%02d.%03d", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    time_t secs = ts.tv_sec;
    localtime_r(&secs, &tm);
    snprintf(out, n, "%02d:%02d:%02d.%03ld", tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000000);
#endif
}

/* A line for the slow-call log: at most one a second, saying how many it held back. */
static void slow_log(uint64_t now, const char* line)
{
    if (g_logged_at && now - g_logged_at < 1000000000ull)
    {
        ++g_held;
        return;
    }
    char when[16];
    clock_text(when, sizeof when);
    if (g_held)
        xi_log("slow %s: %s (and %u more since the last line)", when, line, g_held);
    else
        xi_log("slow %s: %s", when, line);
    g_logged_at = now, g_held = 0;
}

const XiEvent* xi_prof_event(const XiEvent* e)
{
    const XiEvent* was = g_event;
    g_event = e;
    return was;
}

void xi_prof_begin(XiProfMark* m, Addon* a)
{
    m->heap_kb = lua_gc(a->L, LUA_GCCOUNT, 0);
    if (g_depth < (int)(sizeof g_child_wall / sizeof *g_child_wall))
        g_child_wall[g_depth] = g_child_cpu[g_depth] = 0;
    ++g_depth;
    m->cpu = thread_cpu_ns();
    m->wall = rt_monotonic_ns();
}

void xi_prof_end(const XiProfMark* m, Addon* a, const char* where)
{
    uint64_t wall = rt_monotonic_ns() - m->wall, cpu = thread_cpu_ns() - m->cpu;
    int d = --g_depth;
    uint64_t self = wall, self_cpu = cpu;
    if (d < (int)(sizeof g_child_wall / sizeof *g_child_wall))
    {
        self = wall > g_child_wall[d] ? wall - g_child_wall[d] : 0;
        self_cpu = cpu > g_child_cpu[d] ? cpu - g_child_cpu[d] : 0;
    }
    if (d > 0 && d <= (int)(sizeof g_child_wall / sizeof *g_child_wall))
        g_child_wall[d - 1] += wall, g_child_cpu[d - 1] += cpu;
    if (!g_since)
        g_since = m->wall;
    XiProfSlot* s = &a->prof[slot_of(where)];
    s->calls++, s->wall_ns += self, s->cpu_ns += self_cpu;
    if (self > s->max_ns)
        s->max_ns = self;
    a->prof_frame_ns += self;
    g_frame_ns += self;
    if (a->dead)
        return; /* its state is not to be touched again */
    a->heap_kb = lua_gc(a->L, LUA_GCCOUNT, 0);
    uint64_t limit = slow_ns();
    if (limit && self >= limit)
    {
        char line[256], id[16] = "";
        if (g_event && g_event->id && (!strcmp(where, "packet_in") || !strcmp(where, "packet_out")))
            snprintf(id, sizeof id, " 0x%03X", (unsigned)g_event->id);
        snprintf(line, sizeof line, "%s %s%s %.1f ms (cpu %.1f ms, heap %.1f -> %.1f MB)", a->name, where, id, self / 1e6,
            self_cpu / 1e6, m->heap_kb / 1024.0, a->heap_kb / 1024.0);
        slow_log(m->wall + wall, line);
    }
}

void xi_prof_frame(void)
{
    uint64_t now = rt_monotonic_ns(), limit = slow_ns();
    if (g_frame_at && limit && now - g_frame_at >= limit)
    {
        /* the three addons that took longest in it */
        Addon* top[3] = { 0 };
        for (unsigned i = 0; i < xi_addon_count(); ++i)
        {
            Addon* a = xi_addon_at(i);
            for (int j = 0; j < 3; ++j)
                if (a->prof_frame_ns && (!top[j] || a->prof_frame_ns > top[j]->prof_frame_ns))
                {
                    memmove(top + j + 1, top + j, (2 - j) * sizeof *top);
                    top[j] = a;
                    break;
                }
        }
        char line[256];
        int n = snprintf(line, sizeof line, "frame %.0f ms (game thread cpu %.0f ms), addon host %.1f ms, addons' Lua %.1f ms",
            (now - g_frame_at) / 1e6, (thread_cpu_ns() - g_frame_cpu) / 1e6, g_host_ns / 1e6, g_frame_ns / 1e6);
        for (int j = 0; j < 3 && top[j] && n < (int)sizeof line; ++j)
            n += snprintf(line + n, sizeof line - n, "%s %s %.1f", j ? "," : ":", top[j]->name, top[j]->prof_frame_ns / 1e6);
        slow_log(now, line);
    }
    for (unsigned i = 0; i < xi_addon_count(); ++i)
        xi_addon_at(i)->prof_frame_ns = 0;
    g_frame_at = now, g_frame_ns = 0, g_host_ns = 0;
    g_frame_cpu = thread_cpu_ns();
}

void xi_prof_frame_end(void) { g_host_ns += rt_monotonic_ns() - g_frame_at; }

/* --- /addon prof ------------------------------------------------------------------------------ */

/* To chat and to the log, so a bug report's log carries it. */
static void out(const char* fmt, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    xi_chat_write(CHAT_MODE, line);
    xi_log("prof: %s", line);
}

static uint64_t total_ns(const Addon* a)
{
    uint64_t t = 0;
    for (int i = 0; i < XI_PROF_SLOTS; ++i)
        t += a->prof[i].wall_ns;
    return t;
}

static int by_time(const void* x, const void* y)
{
    uint64_t a = total_ns(*(Addon* const*)x), b = total_ns(*(Addon* const*)y);
    return a < b ? 1 : a > b ? -1 : 0;
}

static void show_all(double secs)
{
    unsigned n = xi_addon_count();
    Addon** list = (Addon**)malloc((n ? n : 1) * sizeof *list);
    if (!list)
        return;
    uint64_t sum = 0;
    for (unsigned i = 0; i < n; ++i)
        list[i] = xi_addon_at(i), sum += total_ns(list[i]);
    qsort(list, n, sizeof *list, by_time);
    out("addons' time over %.0f s: %.1f ms a second (%.1f%% of the time)", secs, sum / 1e6 / secs, sum / 1e7 / secs);
    for (unsigned i = 0; i < n && i < 10; ++i)
    {
        Addon* a = list[i];
        uint64_t t = total_ns(a), cpu = 0, max = 0;
        int big[2] = { -1, -1 };
        for (int j = 0; j < XI_PROF_SLOTS; ++j)
        {
            cpu += a->prof[j].cpu_ns;
            if (a->prof[j].max_ns > max)
                max = a->prof[j].max_ns;
            if (!a->prof[j].wall_ns)
                continue;
            if (big[0] < 0 || a->prof[j].wall_ns > a->prof[big[0]].wall_ns)
                big[1] = big[0], big[0] = j;
            else if (big[1] < 0 || a->prof[j].wall_ns > a->prof[big[1]].wall_ns)
                big[1] = j;
        }
        if (!t)
            break;
        char most[96] = "";
        for (int k = 0, m = 0; k < 2 && big[k] >= 0 && m < (int)sizeof most; ++k)
            m += snprintf(most + m, sizeof most - m, "%s%s %.1f", k ? ", " : "", k_slot[big[k]],
                a->prof[big[k]].wall_ns / 1e6 / secs);
        out("  %-12s %6.2f ms/s  max %.1f ms  cpu %.0f%%  heap %.1f MB  (%s)", a->name, t / 1e6 / secs, max / 1e6,
            100.0 * cpu / t, a->heap_kb / 1024.0, most);
    }
    free(list);
}

static void show_one(Addon* a, double secs)
{
    out("%s over %.0f s (heap %.1f MB):", a->name, secs, a->heap_kb / 1024.0);
    for (int j = 0; j < XI_PROF_SLOTS; ++j)
    {
        const XiProfSlot* s = &a->prof[j];
        if (!s->calls)
            continue;
        out("  %-10s %6.2f ms/s  %7.1f calls/s  avg %.1f us  max %.1f ms  cpu %.0f%%", k_slot[j], s->wall_ns / 1e6 / secs,
            s->calls / secs, s->wall_ns / 1e3 / s->calls, s->max_ns / 1e6, s->wall_ns ? 100.0 * s->cpu_ns / s->wall_ns : 0.0);
    }
}

void xi_prof_command(char w[][256], int n)
{
    const char* op = n > 2 ? w[2] : "";
    uint64_t now = rt_monotonic_ns();
    if (!strcasecmp(op, "reset"))
    {
        for (unsigned i = 0; i < xi_addon_count(); ++i)
            memset(xi_addon_at(i)->prof, 0, sizeof xi_addon_at(i)->prof);
        g_since = now;
        out("addon prof: counting from now");
        return;
    }
    if (!strcasecmp(op, "slow"))
    {
        if (n > 3)
            slow_ns(), g_slow_ns = ms_to_ns(atof(w[3]));
        if (slow_ns())
            out("addon prof: calls and frames over %.0f ms go to the log", slow_ns() / 1e6);
        else
            out("addon prof: the slow-call log is off");
        return;
    }
    double secs = g_since && now > g_since ? (now - g_since) / 1e9 : 0;
    if (secs < 0.001)
    {
        out("addon prof: nothing counted yet");
        return;
    }
    if (!*op)
    {
        show_all(secs);
        return;
    }
    Addon* a = xi_addon_find(op);
    if (a)
        show_one(a, secs);
    else
        out("addon prof: %s is not loaded (addon prof [reset | slow <ms> | <name>])", op);
}
