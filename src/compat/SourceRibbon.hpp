// Copyright (C) 2026 WarcraftXL. GPL-3.0-or-later.
#pragma once
#include "engine/assets/shared/models/m2/M2Format.hpp"
#include <array>
#include <cstring>

namespace wxl::modern::assets::m2::material {
// Bounded, owned metadata. Extra texture references are NOT proof of a multiply material.
struct SourceRibbon {
    uint32_t bone=0, textureCount=0, materialCount=0;
    std::array<uint16_t,16> textures{}, materials{};
    int8_t colorIndex=0, transformLookup=-1;
};
inline bool CaptureRibbon(const uint8_t* base,uint32_t size,
    const wxl::structure::m2::M2Ribbon& raw,SourceRibbon& out) noexcept {
    auto copy=[&](wxl::structure::m2::M2Array a,auto& values) {
        if(!a.count||a.count>values.size()||!a.offset||a.offset>size||a.count>(size-a.offset)/2)return false;
        std::memcpy(values.data(),base+a.offset,a.count*2);return true;
    };
    if(!base||!copy(raw.textureIndices,out.textures)||!copy(raw.materialIndices,out.materials))return false;
    out.bone=raw.boneIndex;out.textureCount=raw.textureIndices.count;out.materialCount=raw.materialIndices.count;
    const auto* bytes=reinterpret_cast<const uint8_t*>(&raw);
    std::memcpy(&out.colorIndex,bytes+0xae,1);std::memcpy(&out.transformLookup,bytes+0xaf,1);
    return true;
}
}
