// Copyright (C) 2026 WarcraftXL. GPL-3.0-or-later.
#pragma once
#include <cstdint>
namespace wxl::modern::ribbon {
bool Initialize() noexcept;
// Called after owned source capture, before raw offset relocation. Default-off experiment.
void PrepareSourcePasses(uint8_t* base,uint32_t size) noexcept;
bool Owns(void* emitter) noexcept;
bool Active() noexcept;
void* SetEmitter(void* emitter) noexcept;
using DIPFn=long(__stdcall*)(void*,int,int,unsigned,unsigned,unsigned,unsigned);
long Draw(void* device,int type,int base,unsigned min,unsigned vertices,unsigned start,unsigned primitives,DIPFn original);
}
