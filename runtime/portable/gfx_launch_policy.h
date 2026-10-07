#pragma once
#include <cstdlib>

// The Android renderer's FFXI_ANDROID_* switches ("1" turns one on), set by the launcher before
// gfx_init. With a snapshot they are read once, at configure; without, at every get.
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
    void configure(bool snapshot)
    {
        snapshot_ = snapshot;
        if (snapshot_)
            for (unsigned i = 0; i < Count; ++i)
                values_[i] = read(Flag(i));
    }
    bool get(Flag flag) const { return snapshot_ ? values_[flag] : read(flag); }
    bool snapshot() const { return snapshot_; }

private:
    static bool read(Flag flag)
    {
        const char* p = std::getenv(names_[flag]);
        return p && p[0] == '1' && !p[1];
    }
    inline static constexpr const char* names_[Count] = {"FFXI_ANDROID_CACHE_SAMPLED", "FFXI_ANDROID_BOUNDED_AREA",
                                                         "FFXI_ANDROID_TRIM_UNIFORMS", "FFXI_ANDROID_DONT_CARE_LOADS",
                                                         "FFXI_ANDROID_FAST_SYNC"};
    bool snapshot_ = false;
    bool values_[Count] = {};
};
}
