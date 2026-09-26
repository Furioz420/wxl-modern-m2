// Copyright (C) 2026 WarcraftXL. GPL-3.0-or-later.
#pragma once
#include <cmath>
#include <cstdint>
#include <limits>

namespace wxl_modern_m2::shadowtrace
{
    struct Gate
    {
        uint32_t last = 0, count = 0;
        bool Take(uint32_t now) noexcept
        {
            if (count >= 24 || (count && uint32_t(now - last) < 1500)) return false;
            last = now; ++count; return true;
        }
    };
    // Native 0x829D27..0x829D87 transposes the first three columns of a row-major
    // bone matrix into three float4 constants (including translation in each w).
    inline float PaletteError(const float* matrix, const float* constants) noexcept
    {
        float error = 0;
        for (unsigned c = 0; c < 3; ++c)
            for (unsigned r = 0; r < 4; ++r)
            {
                const float d = std::fabs(matrix[r * 4 + c] - constants[c * 4 + r]);
                if (!std::isfinite(d)) return std::numeric_limits<float>::infinity();
                if (d > error) error = d;
            }
        return error;
    }
}
