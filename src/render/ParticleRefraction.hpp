// Copyright (C) 2026 WarcraftXL. GPL-3.0-or-later.
#pragma once
#include "ParticleLayers.hpp"
namespace wxl::modern::particlelayers {
// A native emitter can grow from one quad to many quads during the same cast.
// Keep native VB/IB, base/min/start and ordering; only the pixel shader changes.
inline bool RefractionQuadBatch(unsigned vertices,unsigned primitives) noexcept {
    return vertices>=4 && vertices<=4096 && vertices%4==0 && primitives==vertices/2;
}
long DrawBloodRefraction(void* device,int type,int base,unsigned min,unsigned vertices,
    unsigned start,unsigned primitives,DIPFn original);
}
