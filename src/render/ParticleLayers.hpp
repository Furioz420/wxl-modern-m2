// Copyright (C) 2026 WarcraftXL. GPL-3.0-or-later.
#pragma once
#include "ParticleDiagnostics.hpp"
namespace wxl::modern::particlelayers {
    bool Initialize() noexcept;
    uint32_t AroundDraw(particlediag::DrawFn original,void* context,void* edx,uint32_t first,
                        void* elements,const uint32_t* order,uint32_t end);
    bool Active() noexcept;
    // Read-only dispatch accounting, before one-shot/ribbon/adapter routing. No device access.
    void ObserveDIP(bool oneShot,bool ribbon) noexcept;
    bool TraceEnabled() noexcept;
    bool TraceTarget() noexcept;
    void* TraceInstance() noexcept;
    using DIPFn=long(__stdcall*)(void*,int,int,unsigned,unsigned,unsigned,unsigned);
    long DrawDIP(void* device,int type,int base,unsigned min,unsigned vertices,unsigned start,unsigned primitives,DIPFn original);
}
