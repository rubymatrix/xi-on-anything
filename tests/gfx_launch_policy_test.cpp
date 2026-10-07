#include "gfx_launch_policy.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
using P = gfxpolicy::LaunchPolicy;
static const char* names[P::Count] = {"FFXI_ANDROID_CACHE_SAMPLED", "FFXI_ANDROID_BOUNDED_AREA",
                                      "FFXI_ANDROID_TRIM_UNIFORMS", "FFXI_ANDROID_DONT_CARE_LOADS",
                                      "FFXI_ANDROID_FAST_SYNC"};
static unsigned checks;
#define REQUIRE(x)                                                                                                     \
    do                                                                                                                 \
    {                                                                                                                  \
        ++checks;                                                                                                      \
        assert(x);                                                                                                     \
    } while (0)
int main()
{
    for (auto n : names)
        unsetenv(n);
    P p;
    REQUIRE(!p.snapshot());
    for (unsigned i = 0; i < P::Count; i++)
        REQUIRE(!p.get(P::Flag(i)));
    const char* values[] = {nullptr, "", "0", "1", "01", "true", "11", "-1"};
    for (const char* value : values)
    {
        for (auto n : names)
        {
            if (value)
                setenv(n, value, 1);
            else
                unsetenv(n);
        }
        const bool expected = value && !std::strcmp(value, "1");
        p.configure(false);
        REQUIRE(!p.snapshot());
        for (unsigned i = 0; i < P::Count; i++)
            REQUIRE(p.get(P::Flag(i)) == expected);
        p.configure(true);
        REQUIRE(p.snapshot());
        for (auto n : names)
            setenv(n, expected ? "0" : "1", 1);
        for (unsigned i = 0; i < P::Count; i++)
            REQUIRE(p.get(P::Flag(i)) == expected);
        for (auto n : names)
            unsetenv(n);
        for (unsigned i = 0; i < P::Count; i++)
            REQUIRE(p.get(P::Flag(i)) == expected);
        p.configure(false);
        for (unsigned i = 0; i < P::Count; i++)
            REQUIRE(!p.get(P::Flag(i)));
    }
    for (unsigned i = 0; i < P::Count; i++)
        setenv(names[i], i % 2 ? "1" : "0", 1);
    p.configure(true);
    for (auto n : names)
        setenv(n, "a different reallocated string", 1);
    for (unsigned i = 0; i < P::Count; i++)
        REQUIRE(p.get(P::Flag(i)) == bool(i % 2));
    p.configure(false);
    for (unsigned i = 0; i < P::Count; i++)
    {
        setenv(names[i], "1", 1);
        REQUIRE(p.get(P::Flag(i)));
        setenv(names[i], "0", 1);
        REQUIRE(!p.get(P::Flag(i)));
    }
    for (auto n : names)
        unsetenv(n);
    std::printf("PASS %u boolean parser/default/dynamic OFF/snapshot ON checks\n", checks);
}
