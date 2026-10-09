#include "gfx_launch_policy.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
using P = gfxpolicy::LaunchPolicy;
static const char* names[P::Count] = {"FFXI_ANDROID_CACHE_SAMPLED", "FFXI_ANDROID_BOUNDED_AREA",
                                      "FFXI_ANDROID_TRIM_UNIFORMS", "FFXI_ANDROID_DONT_CARE_LOADS",
                                      "FFXI_ANDROID_FAST_SYNC"};
int main()
{
    P p;
    for (unsigned i = 0; i < P::Count; i++)
        assert(!p.get(P::Flag(i)));
    const char* values[] = {nullptr, "", "0", "1", "01", "true", "11", "-1"};
    for (const char* value : values)
    {
        for (auto n : names)
            value ? setenv(n, value, 1) : unsetenv(n);
        p.configure();
        for (auto n : names) // read once: later changes are not seen
            setenv(n, "1", 1);
        for (unsigned i = 0; i < P::Count; i++)
            assert(p.get(P::Flag(i)) == (value && !std::strcmp(value, "1")));
    }
    std::printf("PASS\n");
}
