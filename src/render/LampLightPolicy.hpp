#pragma once
#include <cmath>
#include <cstddef>
#include "LampLightCatalog.hpp"

namespace wxl_modern_m2::lamplight
{
    inline bool Matches(const char* path, const char* stem)
    {
        if (!path) return false;
        std::size_t i = 0;
        for (; stem[i]; ++i) {
            char c = path[i];
            if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
            if (c == '\\') c = '/';
            if (c != stem[i]) return false;
        }
        return path[i] == 0 || (path[i] == '.' && (path[i+1] == 'm' || path[i+1] == 'M') &&
            path[i+2] == '2' && path[i+3] == 0);
    }
    inline const Profile* FindProfile(const char* path)
    {
        // All reviewed lamps are world models; reject characters/spells before
        // scanning the catalog in the per-instance animation path.
        if (!path || (path[0]!='w' && path[0]!='W')) return nullptr;
        for (const auto& profile : profiles)
            if (Matches(path,profile.stem)) return &profile;
        return nullptr;
    }
    inline bool Near(float a, float b) { return std::isfinite(a) && std::fabs(a-b) < 0.0001f; }
    inline bool OwnedFalloff(const float* a)
    {
        return (Near(a[0],0) && Near(a[1],.7f) && Near(a[2],.03f)) ||
               (Near(a[0],1) && Near(a[1],.10f) && Near(a[2],.005f));
    }
    // A bounded reciprocal falloff: never amplifies at zero distance. At 5/10 yd,
    // weights are .615/.400 instead of the native torch's .235/.100.
    inline void SetFalloff(float* a) { a[0]=1; a[1]=.10f; a[2]=.005f; }
}
