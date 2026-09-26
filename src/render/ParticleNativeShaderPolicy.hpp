// Copyright (C) 2026 WarcraftXL. GPL-3.0-or-later.
#pragma once
#include "shaders/ParticlePrograms.hpp"
#include <cstddef>
#include <cstring>
namespace wxl::modern::particlelayers {
    // Values also identify native inputs in bounded logs; never infer identity from shader size alone.
    enum class NativeVertexKind : unsigned { Unknown=0,Unlit=1,Directional=2,DirectionalPoint=3,DirectionalTwoPoints=4 };
    inline NativeVertexKind IdentifyNativeVertex(const void* bytes,size_t size) noexcept {
        if(!bytes)return NativeVertexKind::Unknown;
        struct Entry {const uint32_t* bytes;size_t size;NativeVertexKind kind;};
        const Entry entries[]{
            {programs::NativeUnlit,sizeof(programs::NativeUnlit),NativeVertexKind::Unlit},
            {programs::NativeDirectional,sizeof(programs::NativeDirectional),NativeVertexKind::Directional},
            {programs::NativeDirectionalPoint,sizeof(programs::NativeDirectionalPoint),NativeVertexKind::DirectionalPoint},
            {programs::NativeDirectionalTwoPoints,sizeof(programs::NativeDirectionalTwoPoints),NativeVertexKind::DirectionalTwoPoints}
        };
        for(const auto& entry:entries)if(size==entry.size&&!std::memcmp(bytes,entry.bytes,size))return entry.kind;
        return NativeVertexKind::Unknown;
    }
    inline bool NativeStrideMatches(NativeVertexKind kind,unsigned stride) noexcept {
        if(kind==NativeVertexKind::Unlit)return stride==24;
        return stride==36&&(kind==NativeVertexKind::Directional||kind==NativeVertexKind::DirectionalPoint||kind==NativeVertexKind::DirectionalTwoPoints);
    }
}
