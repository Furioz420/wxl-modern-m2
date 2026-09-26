// Copyright (C) 2026 WarcraftXL. GPL-3.0-or-later.
#pragma once
#include <cstdint>
namespace wxl::modern::particlediag
{
    using DrawFn = uint32_t (__fastcall*)(void*, void*, uint32_t, void*, const uint32_t*, uint32_t);
    void Initialize() noexcept;
    // Public wrapper also exercised by standalone guard tests. Always calls original exactly once.
    uint32_t Draw(DrawFn original, void* context, void* edx, uint32_t first,
                  void* elements, const uint32_t* order, uint32_t end);
    void BeforeDIP(void* device, unsigned startIndex, unsigned primitives) noexcept;
}
