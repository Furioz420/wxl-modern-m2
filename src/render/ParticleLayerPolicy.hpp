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
    inline bool DecodeRecipe(const assets::m2::material::ModelSource& model,unsigned index,Recipe& out,bool alphaBlend=false) noexcept {
        if(index>=model.particles.size())return false;
        const auto& p=model.particles[index];
        if(!p.MultiTexture() || (p.Blend()!=7 && !(alphaBlend && p.Blend()==2)) || (p.Flags()&0x80100000u) || index>=model.particleMultipliers.size())return false;
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
    inline bool DecodeNativeSprites(bool enabled,const assets::m2::material::ModelSource& model,unsigned index,Recipe& out) noexcept {
        if(!enabled || model.containerMagic!=wxl::structure::m2::kMagicMD21 || model.innerVersion!=274 ||
           model.globalFlags!=0x3090 || model.particleCapture!=assets::m2::material::ParticleCapture::Captured ||
           index>=model.nativeSpriteLayers.size() || !model.nativeSpriteLayers[index])return false;
        return DecodeRecipe(model,index,out,true);
    }
    // Material-family trial, independent of model name and texture IDs. Unknown
    // metadata is rejected by capture; GPU shader/texture/blend guards still apply.
    // Kind 2 retains native mask UV with approximate extra-layer UV composition.
    inline bool DecodeSpriteMaterials(bool enabled,
        const assets::m2::material::ModelSource& model,unsigned index,Recipe& out) noexcept {
        if(!enabled || model.containerMagic!=wxl::structure::m2::kMagicMD21 ||
           model.innerVersion!=274 || (model.globalFlags!=0x2090 && model.globalFlags!=0x3090) ||
           model.particleCapture!=assets::m2::material::ParticleCapture::Captured ||
           index>=model.spriteLayerKinds.size() ||
           (model.spriteLayerKinds[index]!=1 && model.spriteLayerKinds[index]!=2))return false;
        return DecodeRecipe(model,index,out,true);
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
    // Exact-source experiment: keep native mask UV and use the existing extra-layer
    // scrolling approximation. TXAC's additional UV shader is NOT implemented.
    // Generic admission above remains unchanged and rejects these emitters.
    inline bool DecodeBloodSmokeApprox(bool enabled,bool exactPath,
        const assets::m2::material::ModelSource& model,unsigned index,Recipe& out) noexcept {
        if(!enabled || !exactPath || model.containerMagic!=wxl::structure::m2::kMagicMD21 ||
           model.innerVersion!=274 || model.globalFlags!=0x3090 ||
           model.particleCapture!=assets::m2::material::ParticleCapture::Captured ||
           model.particles.size()!=7 || model.textures.size()!=6 || model.textureFileDataIds.size()!=6 ||
           model.txac11SpriteMask!=0x42 || (index!=1 && index!=2 && index!=5 && index!=6) ||
           model.textureFileDataIds[1]!=1601219 || model.textureFileDataIds[2]!=1602396)return false;
        const auto& p=model.particles[index];
        // These two neutral-TXAC emitters were excluded by the previous smoke-only
        // launcher. Use the existing guarded path, without enabling other models.
        if(index==2 || index==5) {
            if(model.textureFileDataIds[3]!=1601199 || model.textureFileDataIds[4]!=1601200 ||
               p.Flags()!=(index==2?0x70831011u:0x70830011u) || p.Blend()!=7 ||
               p.U16(0x16)!=4227 || p.bytes[0x2c]!=6 || p.bytes[0x2d]!=3)return false;
            return DecodeNativeSprites(true,model,index,out);
        }
        if(p.Flags()!=(index==1?0x70831011u:0x70830011u) || p.Blend()!=2 ||
           p.U16(0x16)!=2113 || p.bytes[0x2c]!=32 || p.bytes[0x2d]!=16)return false;
        return DecodeRecipe(model,index,out,true);
    }
    // Separate opt-in for the captured target disease. The grey bubble emitter
    // remains native. TXAC UV composition is approximate, as for Blood Boil.
    inline bool DecodeBloodPlagueApprox(bool enabled,bool exactPath,
        const assets::m2::material::ModelSource& model,unsigned index,Recipe& out) noexcept {
        if(!enabled || !exactPath || model.containerMagic!=wxl::structure::m2::kMagicMD21 ||
           model.innerVersion!=274 || model.globalFlags!=0x2090 ||
           model.particleCapture!=assets::m2::material::ParticleCapture::Captured ||
           index!=1 || model.particles.size()!=2 || model.textures.size()!=3 ||
           model.textureFileDataIds.size()!=3 || model.textureFileDataIds[0]!=667363 ||
           model.textureFileDataIds[1]!=1601219 || model.textureFileDataIds[2]!=1601220 ||
           model.txac11SpriteMask!=2)return false;
        const auto& p=model.particles[index];
        if(p.Flags()!=0x70830011u || p.Blend()!=2 || p.U16(0x16)!=2113 ||
           p.bytes[0x2c]!=32 || p.bytes[0x2d]!=16)return false;
        const uint16_t scroll[8]={0,32896,0,32896,0,0,0,0};
        for(unsigned i=0;i<8;++i)if(p.U16(0x1dc+2*i)!=scroll[i])return false;
        return DecodeRecipe(model,index,out,true);
    }
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
