#pragma once
#include <errno.h>
#include <stdlib.h>

/* d3d8.c's Android shadow-map diagnostics, from the launch environment: FFXI_ANDROID_SKIP_GAME_SHADOW_PASSES,
 * FFXI_ANDROID_SHADOW_MAP_BUDGET (0-3) and FFXI_ANDROID_SHADOW_MAP_INTERVAL (1-4). With a snapshot they
 * are read once, at configure, before d3d8.c registers its thunks; without, at every call. A snapshot
 * also keeps the errno strtol left, and sets it again on each read, as a live parse would. */
typedef struct XiFrontendPolicy
{
    int snapshot, skip, budget, interval;
    int budget_errno, interval_errno;
} XiFrontendPolicy;

static inline int xi_frontend_skip_read(void)
{
    const char* p = getenv("FFXI_ANDROID_SKIP_GAME_SHADOW_PASSES");
    return p && p[0] == '1' && !p[1];
}
static inline int xi_frontend_budget_read(void)
{
    const char* p = getenv("FFXI_ANDROID_SHADOW_MAP_BUDGET");
    if (!p || !*p)
        return 0;
    char* end = NULL;
    long n = strtol(p, &end, 10);
    return end != p && !*end && n >= 0 && n <= 3 ? (int)n : 0;
}
static inline int xi_frontend_interval_read(void)
{
    const char* p = getenv("FFXI_ANDROID_SHADOW_MAP_INTERVAL");
    if (!p || !*p)
        return 1;
    char* end = NULL;
    long n = strtol(p, &end, 10);
    return end != p && !*end && n >= 1 && n <= 4 ? (int)n : 1;
}
static inline void xi_frontend_policy_configure(XiFrontendPolicy* p, int snapshot)
{
    *p = (XiFrontendPolicy){0};
    p->snapshot = snapshot != 0;
    if (!p->snapshot)
        return;
    int saved_errno = errno;
    p->skip = xi_frontend_skip_read();
    errno = 0;
    p->budget = xi_frontend_budget_read();
    p->budget_errno = errno;
    errno = 0;
    p->interval = xi_frontend_interval_read();
    p->interval_errno = errno;
    errno = saved_errno;
}
static inline int xi_frontend_skip(const XiFrontendPolicy* p)
{
    return p->snapshot ? p->skip : xi_frontend_skip_read();
}
static inline int xi_frontend_budget(const XiFrontendPolicy* p)
{
    if (!p->snapshot)
        return xi_frontend_budget_read();
    if (p->budget_errno)
        errno = p->budget_errno;
    return p->budget;
}
static inline int xi_frontend_interval(const XiFrontendPolicy* p)
{
    if (!p->snapshot)
        return xi_frontend_interval_read();
    if (p->interval_errno)
        errno = p->interval_errno;
    return p->interval;
}
static inline int xi_frontend_diagnostic(const XiFrontendPolicy* p)
{
    return xi_frontend_skip(p) || xi_frontend_budget(p) > 0;
}
