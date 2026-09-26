// Copyright (C) 2026 WarcraftXL. GPL-3.0-or-later.
#pragma once
#include <cstdint>
#include <vector>
namespace wxl::modern::particlelayers {
inline bool QuadShape(unsigned vertices,unsigned primitives) noexcept {
    return vertices && vertices<=4096 && vertices%4==0 && primitives==vertices/2;
}
// Native allocations may sit anywhere in the VB/IB ring. Check their original ranges,
// then submit owned buffers with base/min/start zero; never reuse a native base on a new VB.
inline bool QuadRanges(int base,unsigned min,unsigned vertices,unsigned start,unsigned primitives,
    unsigned offset,unsigned stride,unsigned vertexBytes,unsigned indexBytes) noexcept {
    if(base<0 || min || !QuadShape(vertices,primitives) || (stride!=24&&stride!=36))return false;
    const uint64_t vertexEnd=uint64_t(offset)+(uint64_t(unsigned(base))+vertices)*stride;
    const uint64_t indexEnd=(uint64_t(start)+uint64_t(primitives)*3)*2;
    return vertexEnd<=vertexBytes && indexEnd<=indexBytes;
}
inline bool QuadIndices(unsigned vertices,std::vector<uint16_t>& out) {
    if(!vertices||vertices>4096||vertices%4)return false;
    std::vector<uint16_t> result;result.reserve(vertices/4*6);
    // CParticleEmitter2::RenderIndices: EXACT native quad order, not a guessed fan.
    for(unsigned first=0;first<vertices;first+=4)
        for(unsigned corner : {0u,1u,2u,3u,2u,1u})result.push_back(uint16_t(first+corner));
    out.swap(result);return true;
}
}
