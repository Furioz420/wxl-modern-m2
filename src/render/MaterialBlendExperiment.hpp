// Copyright (C) 2026 WarcraftXL. GPL-3.0-or-later.
#pragma once
namespace wxl::modern::materialblend
{
    using DrawFn = long (__stdcall*)(void*, int, int, unsigned, unsigned, unsigned, unsigned);
    void Initialize() noexcept;
    long Draw(void* device, void* context, void* instance, int type, int baseVertex,
              unsigned minVertex, unsigned vertices, unsigned start, unsigned primitives,
              DrawFn original);
}
