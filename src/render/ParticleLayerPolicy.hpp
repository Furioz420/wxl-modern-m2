// Copyright (C) 2026 WarcraftXL. GPL-3.0-or-later.
#pragma once
#include "ParticleShaderContract.hpp"
#include "../compat/SourceMaterialCapture.hpp"
#include <cstring>
namespace wxl::modern::particlelayers {
    namespace pm=particlematerial;
    // wowdev fixed_point<uint16_t,6,9>: sign-magnitude, not two's complement.
    inline float Scroll(uint16_t raw) noexcept {return float(raw&0x7fff)*(raw&0x8000?-1.0f:1.0f)/512.0f;}
    struct Recipe { pm::Combine combine; std::array<uint16_t,3> indices; std::array<float,2> scale,multipliers; std::array<pm::Vec2,2> mid,range; };
    // Structural recipe extraction only. Callers MUST first establish their admission contract.
    inline bool DecodeRecipe(const assets::m2::material::ModelSource& model,unsigned index,Recipe& out) noexcept {
        if(index>=model.particles.size())return false;
        const auto& p=model.particles[index];
        if(!p.MultiTexture() || p.Blend()!=7 || (p.Flags()&0x80100000u) || index>=model.particleMultipliers.size())return false;
        Recipe r{};r.combine=(p.Flags()&0x40000000u)?pm::Combine::ThreeColorThreeAlpha:pm::Combine::TwoColorThreeAlpha;
        r.multipliers=model.particleMultipliers[index];
        for(unsigned stage=0;stage<3;++stage) {
            r.indices[stage]=p.Texture(stage);
            if(r.indices[stage]>=model.textures.size() || model.textures[r.indices[stage]].type!=0)return false;
        }
        for(unsigned layer=0;layer<2;++layer) {
            // Signed 8-bit scale remains excluded; 16-bit scroll is sign-magnitude.
            if(p.bytes[0x2c+layer]&0x80)return false;
            r.scale[layer]=p.bytes[0x2c+layer]/32.0f;
            const unsigned at=0x1dc+layer*4;
            r.mid[layer]={Scroll(p.U16(at)),Scroll(p.U16(at+2))};
            r.range[layer]={Scroll(p.U16(at+8)),Scroll(p.U16(at+10))};
        }
        out=r;return true;
    }
    inline bool Decode(const assets::m2::material::ModelSource& model,unsigned index,Recipe& out) noexcept {
        namespace mat=assets::m2::material;
        if(model.containerMagic!=wxl::structure::m2::kMagicMD21 || model.innerVersion!=274 || !model.particleLayerFeaturesKnown ||
            (model.globalFlags&0x1000) || model.particleCapture!=mat::ParticleCapture::Captured)return false;
        return DecodeRecipe(model,index,out);
    }
    inline bool DecodeRiftNativeSprites(bool enabled,bool exactPath,
        const assets::m2::material::ModelSource& model,unsigned index,Recipe& out) noexcept {
        namespace mat=assets::m2::material;
        // Independent opt-in experiment. Keep generic Decode fail-closed, including
        // unknown global flags. This is not permission for arbitrary modern emitters.
        if(!enabled || !exactPath || model.riftNativeSpriteMask!=0x21f || index>=13 ||
           !(model.riftNativeSpriteMask&(1u<<index)) ||
           model.containerMagic!=wxl::structure::m2::kMagicMD21 || model.innerVersion!=274 ||
           model.globalFlags!=0x203090 || model.particleCapture!=mat::ParticleCapture::Captured ||
           model.particles.size()!=13 || model.textures.size()!=27 || model.particleMultipliers.size()!=13)return false;
        return DecodeRecipe(model,index,out);
    }
    inline uint32_t Mix(uint32_t x) noexcept { x^=x>>16;x*=0x7feb352du;x^=x>>15;x*=0x846ca68bu;return x^(x>>16); }
    inline float Unit(uint32_t x) noexcept {return float(Mix(x)>>8)*(1.0f/16777216.0f);}
    // Experimental deterministic per-slot randomization; never advances the engine RNG.
    // Reused pool slots repeat their seed. This is NOT donor RNG sequence parity.
    inline pm::LayerMotion Motion(const Recipe& recipe,unsigned layer,uint32_t seed) noexcept {
        const uint32_t salt=seed+0x9e3779b9u*(layer+1);
        const float random=2*Unit(salt+2)-1;
        return {{Unit(salt),Unit(salt+1)},
            {recipe.mid[layer].x+recipe.range[layer].x*random,recipe.mid[layer].y+recipe.range[layer].y*random},
            {recipe.scale[layer],recipe.scale[layer]}};
    }
    inline bool ExpandQuad(const uint8_t* native,unsigned stride,float age,uint32_t seed,
                           const Recipe& recipe,uint8_t* output) noexcept {
        if(!native || !output || (stride!=24 && stride!=36))return false;
        std::array<pm::Vec2,4> uv{};
        for(unsigned i=0;i<4;++i){std::memcpy(&uv[i],native+i*stride+stride-8,8);if(!std::isfinite(uv[i].x)||!std::isfinite(uv[i].y))return false;}
        pm::Vec2 lo=uv[0],hi=uv[0];
        for(auto v:uv){lo.x=v.x<lo.x?v.x:lo.x;lo.y=v.y<lo.y?v.y:lo.y;hi.x=v.x>hi.x?v.x:hi.x;hi.y=v.y>hi.y?v.y:hi.y;}
        if(hi.x-lo.x<1e-7f || hi.y-lo.y<1e-7f)return false;
        std::array<uint8_t,4*52> scratch{};
        unsigned corners=0;
        for(unsigned i=0;i<4;++i){
            const pm::Vec2 corner{(uv[i].x-lo.x)/(hi.x-lo.x),(uv[i].y-lo.y)/(hi.y-lo.y)};
            const unsigned x=corner.x>.5f?1:0,y=corner.y>.5f?1:0;
            if(std::fabs(corner.x-float(x))>1e-4f||std::fabs(corner.y-float(y))>1e-4f||(corners&(1u<<(x+2*y))))return false;
            corners|=1u<<(x+2*y);
            auto* dst=scratch.data()+i*(stride+16);std::memcpy(dst,native+i*stride,stride);
            for(unsigned layer=0;layer<2;++layer){pm::Vec2 extra;
                if(!pm::EvaluateUV(corner,Motion(recipe,layer,seed),age,extra))return false;
                std::memcpy(dst+stride+layer*8,&extra,8);}
        }
        std::memcpy(output,scratch.data(),4*(stride+16));return true;
    }
}
