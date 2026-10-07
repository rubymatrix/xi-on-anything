#pragma once
#include <stdlib.h>

/* d3d8.c's Android shadow-map diagnostics, from the launch environment and read once, before d3d8.c
 * registers its thunks: FFXI_ANDROID_SKIP_GAME_SHADOW_PASSES, FFXI_ANDROID_SHADOW_MAP_BUDGET (0-3) and
 * FFXI_ANDROID_SHADOW_MAP_INTERVAL (1-4). */
typedef struct XiFrontendPolicy
{
    int skip, budget, interval;
} XiFrontendPolicy;

/* a whole decimal number in [lo, hi], else fallback */
static inline int xi_frontend_number(const char* name, int lo, int hi, int fallback)
{
    const char* p = getenv(name);
    if (!p || !*p)
        return fallback;
    char* end = NULL;
    long n = strtol(p, &end, 10);
    return end != p && !*end && n >= lo && n <= hi ? (int)n : fallback;
}
static inline XiFrontendPolicy xi_frontend_policy_read(void)
{
    const char* skip = getenv("FFXI_ANDROID_SKIP_GAME_SHADOW_PASSES");
    XiFrontendPolicy p = {skip && skip[0] == '1' && !skip[1],
                          xi_frontend_number("FFXI_ANDROID_SHADOW_MAP_BUDGET", 0, 3, 0),
                          xi_frontend_number("FFXI_ANDROID_SHADOW_MAP_INTERVAL", 1, 4, 1)};
    return p;
}
