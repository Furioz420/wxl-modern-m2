// Copyright (C) 2026 WarcraftXL. GPL-3.0-or-later.
#pragma once
#include <array>
#include <cstdint>
#include <cstring>
namespace wxl::modern::particlediag
{
    // The native particle function returns how many ADDITIONAL sorted elements it consumed.
    // Limiting the exclusive end to first+1 leaves one element, so the outer loop advances normally.
    inline uint32_t IsolatedEnd(bool isolate, uint32_t first, uint32_t end,
                                const void* elements, const void* order) noexcept
    {
        return isolate && elements && order && first < end && end <= 0x10000 ? first + 1 : end;
    }
    inline bool EmitterIndex(uintptr_t base, uint32_t count, uintptr_t headCells, uint32_t& out) noexcept
    {
        constexpr uintptr_t headOffset = 0x13C, stride = 0x1DC;
        if (!base || !count || count > 2048 || headCells < headOffset) return false;
        const auto record = headCells - headOffset;
        if (record < base) return false;
        const auto delta = record - base;
        if (delta % stride || delta / stride >= count) return false;
        out = uint32_t(delta / stride);
        return true;
    }
    // Small exact-byte deduplication; never use a hash collision to suppress different programs.
    struct ShaderBytes
    {
        static constexpr uint32_t kMax = 4096;
        std::array<uint8_t, kMax> data{};
        uint32_t size = 0;
    };
    template<class Shader> bool ReadShader(Shader* shader, ShaderBytes& bytes) noexcept
    {
        bytes.size = 0;
        if (!shader) return false;
        unsigned size = 0;
        if (shader->GetFunction(nullptr, &size) < 0 || !size || size > bytes.data.size() || size % 4) return false;
        unsigned copied = size;
        if (shader->GetFunction(bytes.data.data(), &copied) < 0 || copied != size) return false;
        bytes.size = size;
        return true;
    }
    class ShaderSet
    {
        std::array<ShaderBytes, 8> entries_{};
        unsigned used_ = 0;
    public:
        // 0 = out of budget/invalid; ID remains stable for exact repeated bytes.
        unsigned Insert(const ShaderBytes& bytes, bool& added) noexcept
        {
            added = false;
            if (!bytes.size || bytes.size > bytes.data.size()) return 0;
            for (unsigned i = 0; i < used_; ++i)
                if (entries_[i].size == bytes.size && !std::memcmp(entries_[i].data.data(), bytes.data.data(), bytes.size)) return i+1;
            if (used_ == entries_.size()) return 0;
            entries_[used_++] = bytes;
            added = true;
            return used_;
        }
    };
}
