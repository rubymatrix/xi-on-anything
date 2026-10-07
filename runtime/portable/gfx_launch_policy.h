#pragma once
#include <cstdlib>

// The Android renderer's FFXI_ANDROID_* switches ("1" turns one on), set by the launcher before
// gfx_init and read once there, not per draw.
namespace gfxpolicy
{
class LaunchPolicy
{
public:
    enum Flag
    {
        SampledCache,
        BoundedArea,
        TrimUniforms,
        DontCareLoads,
        FastSync,
        Count
    };
    void configure()
    {
        for (unsigned i = 0; i < Count; ++i)
        {
            const char* p = std::getenv(names_[i]);
            values_[i] = p && p[0] == '1' && !p[1];
        }
    }
    bool get(Flag flag) const { return values_[flag]; }

private:
    inline static constexpr const char* names_[Count] = {"FFXI_ANDROID_CACHE_SAMPLED", "FFXI_ANDROID_BOUNDED_AREA",
                                                         "FFXI_ANDROID_TRIM_UNIFORMS", "FFXI_ANDROID_DONT_CARE_LOADS",
                                                         "FFXI_ANDROID_FAST_SYNC"};
    bool values_[Count] = {};
};
}
