// Source-owned position before the native engine can reuse the borrowed vertex array.
#pragma once
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
namespace wxl_modern_m2::vertexorigin
{
    struct Origin { uint32_t count = 0; float position[3]{}; };
    inline bool Read(const uint8_t* body, size_t size, uint32_t offset, uint32_t count, Origin& out)
    {
        constexpr uint64_t stride = 48;
        if (!body || !count || !offset || offset > size ||
            uint64_t(count) * stride > size - offset) return false;
        Origin value{};
        value.count = count;
        std::memcpy(value.position, body + offset, sizeof value.position);
        for (float p : value.position) if (!std::isfinite(p)) return false;
        out = value;
        return true;
    }
    inline bool Matches(const Origin& value, uint32_t count) { return count && value.count == count; }
}
