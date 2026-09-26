// Copyright (C) 2026 WarcraftXL. GPL-3.0-or-later.
#pragma once
#include <cstdint>

namespace wxl_modern_m2::shadowindex
{
    // Test 06 A/B validated the guarded repair in-world. Explicit 0/1 remain available
    // for diagnosis; ordinary launches should not revert to truncated shadow windows.
    inline constexpr uint32_t kDefaultMode = 2;
    struct Input
    {
        bool modern = false, extended = false, triangles = false;
        uint32_t runCount = 0, level = 0, low = 0, sectionCount = 0;
        uint32_t start = 0, primitives = 0, skinIndices = 0;
    };
    inline uint32_t Candidate(const Input& in) noexcept
    {
        if (!in.modern || !in.extended || !in.triangles || in.runCount != 1 ||
            !in.level || in.level > 0xffff || in.low > 0xffff || in.start != in.low ||
            !in.sectionCount || uint64_t(in.primitives)*3 != in.sectionCount)
            return in.start;
        const uint32_t full = (in.level << 16) | in.low;
        return uint64_t(full)+in.sectionCount <= in.skinIndices ? full : in.start;
    }
    inline bool FitsBuffer(uint32_t start, uint32_t count, uint32_t bytes, uint32_t indexBytes) noexcept
    {
        return (indexBytes==2 || indexBytes==4) && bytes%indexBytes==0 &&
            (uint64_t(start)+count)*indexBytes <= bytes;
    }
}
