#define _POSIX_C_SOURCE 200809L
#include "android_frontend_policy.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned checks;
#define CHECK(x)                                                                                                       \
    do                                                                                                                 \
    {                                                                                                                  \
        ++checks;                                                                                                      \
        assert(x);                                                                                                     \
    } while (0)
/* Independent oracle: parse the environment on every call, as the policy must match. */
static int old_budget(void)
{
    const char* p = getenv("FFXI_ANDROID_SHADOW_MAP_BUDGET");
    if (!p || !*p)
        return 0;
    char* end = NULL;
    long n = strtol(p, &end, 10);
    return end != p && !*end && n >= 0 && n <= 3 ? (int)n : 0;
}
static int old_interval(void)
{
    const char* p = getenv("FFXI_ANDROID_SHADOW_MAP_INTERVAL");
    if (!p || !*p)
        return 1;
    char* end = NULL;
    long n = strtol(p, &end, 10);
    return end != p && !*end && n >= 1 && n <= 4 ? (int)n : 1;
}
static int old_skip(void)
{
    const char* p = getenv("FFXI_ANDROID_SKIP_GAME_SHADOW_PASSES");
    return p && p[0] == '1' && !p[1];
}
static int old_diagnostic(void)
{
    return old_skip() || old_budget() > 0;
}
static const char* names[] = {"FFXI_ANDROID_SKIP_GAME_SHADOW_PASSES", "FFXI_ANDROID_SHADOW_MAP_BUDGET",
                              "FFXI_ANDROID_SHADOW_MAP_INTERVAL"};
static void set(unsigned i, const char* v)
{
    if (v)
        CHECK(setenv(names[i], v, 1) == 0);
    else
        CHECK(unsetenv(names[i]) == 0);
}
typedef int (*Old)(void);
typedef int (*New)(const XiFrontendPolicy*);
static Old old[] = {old_skip, old_budget, old_interval, old_diagnostic};
static New get[] = {xi_frontend_skip, xi_frontend_budget, xi_frontend_interval, xi_frontend_diagnostic};
int main(void)
{
    const char* values[] = {NULL,
                            "",
                            "0",
                            "1",
                            "2",
                            "3",
                            "4",
                            "5",
                            "-1",
                            "-0",
                            "+1",
                            " 1",
                            "\t2",
                            "1 ",
                            "1x",
                            "01",
                            "0x1",
                            "true",
                            "99999999999999999999999999999999999",
                            "-99999999999999999999999999999999999"};
    XiFrontendPolicy p = {0};
    for (unsigned i = 0; i < 3; i++)
        set(i, NULL);
    CHECK(!p.snapshot);
    CHECK(xi_frontend_diagnostic(&p) == 0);
    CHECK(xi_frontend_interval(&p) == 1);
    for (unsigned s = 0; s < sizeof(values) / sizeof(*values); s++)
        for (unsigned b = 0; b < sizeof(values) / sizeof(*values); b++)
            for (unsigned in = 0; in < sizeof(values) / sizeof(*values); in++)
            {
                set(0, values[s]);
                set(1, values[b]);
                set(2, values[in]);
                int expected[4], errors[4];
                for (unsigned j = 0; j < 4; j++)
                {
                    errno = E2BIG;
                    expected[j] = old[j]();
                    errors[j] = errno;
                }
                for (int mode = 0; mode <= 1; mode++)
                {
                    errno = E2BIG;
                    xi_frontend_policy_configure(&p, mode);
                    CHECK(errno == E2BIG);
                    CHECK(p.snapshot == mode);
                    for (unsigned j = 0; j < 4; j++)
                    {
                        errno = E2BIG;
                        CHECK(get[j](&p) == expected[j]);
                        CHECK(errno == errors[j]);
                    }
                }
                set(0, "changed and reallocated");
                set(1, "3");
                set(2, NULL);
                for (unsigned j = 0; j < 4; j++)
                {
                    errno = E2BIG;
                    CHECK(get[j](&p) == expected[j]);
                    CHECK(errno == errors[j]);
                }
                xi_frontend_policy_configure(&p, 0);
                for (unsigned j = 0; j < 4; j++)
                {
                    errno = EDOM;
                    int expected_now = old[j](), error_now = errno;
                    errno = EDOM;
                    CHECK(get[j](&p) == expected_now);
                    CHECK(errno == error_now);
                }
            }
    /* Reinitialization must replace an earlier process policy snapshot. */
    set(0, "1");
    set(1, "2");
    set(2, "4");
    xi_frontend_policy_configure(&p, 1);
    set(0, NULL);
    set(1, NULL);
    set(2, NULL);
    xi_frontend_policy_configure(&p, 1);
    CHECK(xi_frontend_skip(&p) == 0);
    CHECK(xi_frontend_budget(&p) == 0);
    CHECK(xi_frontend_interval(&p) == 1);
    printf("PASS %u original-parser/errno/default/dynamic-OFF/snapshot-ON/reinit checks\n", checks);
}
