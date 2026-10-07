#define _POSIX_C_SOURCE 200809L
#include "android_frontend_policy.h"
#include <assert.h>
#include <stdio.h>

static void set(const char* name, const char* v)
{
    if (v)
        setenv(name, v, 1);
    else
        unsetenv(name);
}
int main(void)
{
    static const struct
    {
        const char* value;
        int skip, budget, interval;
    } cases[] = {
        {NULL, 0, 0, 1}, {"", 0, 0, 1},   {"0", 0, 0, 1},  {"1", 1, 1, 1},     {"2", 0, 2, 2},
        {"3", 0, 3, 3},  {"4", 0, 0, 4},  {"5", 0, 0, 1},  {"-1", 0, 0, 1},    {"+1", 0, 1, 1},
        {"01", 0, 1, 1}, {"1 ", 0, 0, 1}, {"1x", 0, 0, 1}, {"true", 0, 0, 1},  {"99999999999999999999", 0, 0, 1},
    };
    for (unsigned i = 0; i < sizeof cases / sizeof *cases; i++)
    {
        set("FFXI_ANDROID_SKIP_GAME_SHADOW_PASSES", cases[i].value);
        set("FFXI_ANDROID_SHADOW_MAP_BUDGET", cases[i].value);
        set("FFXI_ANDROID_SHADOW_MAP_INTERVAL", cases[i].value);
        XiFrontendPolicy p = xi_frontend_policy_read();
        assert(p.skip == cases[i].skip && p.budget == cases[i].budget && p.interval == cases[i].interval);
    }
    printf("PASS %u values\n", (unsigned)(sizeof cases / sizeof *cases));
}
