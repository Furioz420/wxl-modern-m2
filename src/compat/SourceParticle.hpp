// Copyright (C) 2026 WarcraftXL. GPL-3.0-or-later.
#pragma once
#include <array>
#include <cstdint>
#include <cstring>

namespace wxl::modern::assets::m2::material
{
    enum class ParticleCapture : uint8_t { Absent, Captured, UnsupportedVersion, InvalidRange, OverBudget };
    // Original source record, including the modern tail. Array offsets inside are FILE offsets,
    // never live pointers. Referenced animation arrays are not duplicated by this record copy.
    struct SourceParticle
    {
        static constexpr uint32_t kStride = 0x1EC;
        static constexpr uint32_t kMultiTexture = 0x10000000;
        std::array<uint8_t, kStride> bytes{};
        uint16_t U16(size_t at) const noexcept
        {
            uint16_t value = 0;
            if (at <= bytes.size() - sizeof(value)) std::memcpy(&value, bytes.data() + at, sizeof(value));
            return value;
        }
        uint32_t U32(size_t at) const noexcept
        {
            uint32_t value = 0;
            if (at <= bytes.size() - sizeof(value)) std::memcpy(&value, bytes.data() + at, sizeof(value));
            return value;
        }
        uint32_t Flags() const noexcept { return U32(4); }
        uint8_t Blend() const noexcept { return bytes[0x28]; }
        bool MultiTexture() const noexcept { return (Flags() & kMultiTexture) != 0; }
        uint16_t Texture(unsigned stage) const noexcept
        {
            if (stage >= (MultiTexture() ? 3u : 1u)) return 0xFFFF;
            return MultiTexture() ? uint16_t((U16(0x16) >> (stage * 5)) & 31) : U16(0x16);
        }
    };
    static_assert(sizeof(SourceParticle) == SourceParticle::kStride);
}
