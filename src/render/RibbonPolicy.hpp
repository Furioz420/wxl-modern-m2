// Copyright (C) 2026 WarcraftXL. GPL-3.0-or-later.
#pragma once
#include "../compat/SourceMaterialCapture.hpp"
#include <cmath>
namespace wxl::modern::ribbon {
struct Recipe {unsigned texture=0,transform=0;};
struct LayerRecipe {std::array<unsigned,3> textures{},transforms{},flags{};};
inline bool Decode(const assets::m2::material::ModelSource& s,unsigned index,Recipe& out) noexcept {
    // Initial unlit/unfogged alpha-blended subset. No name-based production eligibility.
    if(s.containerMagic!=wxl::structure::m2::kMagicMD21||s.innerVersion!=274||
        !(s.globalFlags&0x20000)||!s.ribbonsCaptured||index>=s.ribbons.size())return false;
    const auto& r=s.ribbons[index];
    if(r.materialCount!=1||!r.textureCount||r.textureCount>16||r.colorIndex!=0||r.transformLookup<0||
        r.materials[0]>=s.materials.size()||unsigned(r.transformLookup)>=s.transformCombos.size())return false;
    const auto& m=s.materials[r.materials[0]];
    if(m.flags!=0x17||m.blend!=2)return false;
    for(unsigned n=0;n<r.textureCount;++n)
        if(r.textures[n]>=s.textures.size()||s.textures[r.textures[n]].type!=0)return false;
    out.texture=r.textures[0];out.transform=s.transformCombos[unsigned(r.transformLookup)];
    return out.transform==0xffff || out.transform<s.sourceTransformCount;
}
// Opt-in candidate only: three authored maps and consecutive transform-combo slots.
// This is not a recovered donor shader selector; keep separate from Decode's proven subset.
inline bool DecodeLayers(const assets::m2::material::ModelSource& s,unsigned index,LayerRecipe& out) noexcept {
    Recipe first;if(!Decode(s,index,first))return false;
    const auto& r=s.ribbons[index];
    const unsigned lookup=unsigned(r.transformLookup);
    if(r.textureCount!=3||s.transformCombos.size()-lookup<3)return false;
    LayerRecipe candidate;
    for(unsigned n=0;n<3;++n){
        candidate.textures[n]=r.textures[n];candidate.transforms[n]=s.transformCombos[lookup+n];
        candidate.flags[n]=s.textures[r.textures[n]].flags;
        if((candidate.flags[n]&~3u)||(candidate.transforms[n]!=0xffff&&candidate.transforms[n]>=s.sourceTransformCount))return false;
    }
    out=candidate;return true;
}
// Native CShaderEffect::SetTexMtx uploads (m0,m4,m8,m12), (m1,m5,m9,m13).
// For UV=(u,v,0,1), the diagonal subset is (m0*u+m12, m5*v+m13).
// Reject rotation/shear instead of silently discarding cross terms. Z scale may legitimately be 0.
inline bool NativeScaleOffset(const float* matrix,float (&scaleOffset)[4]) noexcept {
    for(unsigned n=0;n<16;++n)if(!std::isfinite(matrix[n]))return false;
    if(matrix[1]!=0||matrix[4]!=0||matrix[15]!=1)return false;
    scaleOffset[0]=matrix[0];scaleOffset[1]=matrix[5];
    scaleOffset[2]=matrix[12];scaleOffset[3]=matrix[13];
    return true;
}
// Retained only for the older reference-renderer mode 2; NOT the WotLK matrix adapter.
inline bool Transform(const float* matrix,float (&scaleOffset)[4]) noexcept {
    for(unsigned n=0;n<16;++n)if(!std::isfinite(matrix[n]))return false;
    // Reference ribbon shader: lengths of matrix axes 0 and 2, translation xy.
    scaleOffset[0]=std::sqrt(matrix[0]*matrix[0]+matrix[1]*matrix[1]+matrix[2]*matrix[2]);
    scaleOffset[1]=std::sqrt(matrix[8]*matrix[8]+matrix[9]*matrix[9]+matrix[10]*matrix[10]);
    scaleOffset[2]=matrix[12];scaleOffset[3]=matrix[13];
    return std::isfinite(scaleOffset[0])&&std::isfinite(scaleOffset[1]);
}
}
