// Copyright (C) 2026 WarcraftXL. GPL-3.0-or-later.
#pragma once
#include <array>
#include <cstdint>
#include <cstring>

namespace wxl::modern::materialdiag
{
    enum class ShaderRead { NoShader, SizeFailed, OverBudget, ReadFailed, SizeChanged, Read };
    inline const char* ShaderReadName(ShaderRead state) noexcept
    {
        switch (state)
        {
        case ShaderRead::NoShader: return "no-interface";
        case ShaderRead::SizeFailed: return "size-query-failed";
        case ShaderRead::OverBudget: return "size-outside-budget";
        case ShaderRead::ReadFailed: return "read-failed";
        case ShaderRead::SizeChanged: return "size-changed";
        case ShaderRead::Read: return "read";
        }
        return "unknown";
    }
    struct ShaderIdentity
    {
        ShaderRead state = ShaderRead::NoShader;
        int32_t sizeHr = 0, dataHr = 0;
        unsigned bytes = 0;
        uint32_t hash = 0; // FNV-1a diagnostic grouping only; never evidence of exact equality
        bool exactKnown = false;
    };
    template<class Shader>
    ShaderIdentity InspectShader(Shader* shader, const uint8_t* known, size_t knownSize)
    {
        ShaderIdentity out;
        if (!shader) return out;
        out.sizeHr = static_cast<int32_t>(shader->GetFunction(nullptr, &out.bytes));
        if (out.sizeHr < 0) { out.state = ShaderRead::SizeFailed; return out; }
        if (!out.bytes || out.bytes > 16384) { out.state = ShaderRead::OverBudget; return out; }
        std::array<uint8_t, 16384> buffer{};
        unsigned returned = out.bytes;
        out.dataHr = static_cast<int32_t>(shader->GetFunction(buffer.data(), &returned));
        if (out.dataHr < 0) { out.state = ShaderRead::ReadFailed; return out; }
        if (returned != out.bytes) { out.state = ShaderRead::SizeChanged; return out; }
        out.state = ShaderRead::Read;
        out.hash = 2166136261u;
        for (unsigned n = 0; n < returned; ++n) out.hash = (out.hash ^ buffer[n]) * 16777619u;
        out.exactKnown = known && knownSize == returned && !std::memcmp(buffer.data(), known, returned);
        return out;
    }
}
