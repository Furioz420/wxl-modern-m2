// Copyright (C) 2026 WarcraftXL. GPL-3.0-or-later.
#pragma once
#include "SourceMaterialCapture.hpp"
#include <cmath>
#include <cstring>

namespace wxl::modern::assets::m2::material {
// Conservative experiment subset, not a general EXP2/TXAC implementation.
// All offsets are checked against their own chunk/body before access.
inline bool CaptureLayerFeatures(const uint8_t* body,uint32_t bodySize,
    const uint8_t* container,uint32_t size,ModelSource& model) {
    auto u32=[](const uint8_t* p){uint32_t v;std::memcpy(&v,p,4);return v;};
    auto fits=[](uint32_t at,uint32_t count,uint32_t stride,uint32_t limit){return at<=limit&&count<=(limit-at)/stride;};
    if(model.particleCapture!=ParticleCapture::Captured)return false;
    for(const auto& p:model.particles)for(unsigned field : {0x18u,0x20u}) {
        const auto count=p.U32(field),offset=p.U32(field+4);
        // Some exporters encode an empty name as a single NUL, not count=0.
        if(count && (count!=1||!offset||offset>=bodySize||body[offset]!=0))return false;
    }
    model.particleMultipliers.assign(model.particles.size(),{1.0f,1.0f});
    if(!container)return model.skippedChunks==0; // synthetic/known body-only capture
    uint32_t seen=0,at=0;
    while(at<=size&&size-at>=8) {
        const auto length=u32(container+at+4);
        if(length>size-at-8)return false;
        const auto* tag=container+at;const auto* data=tag+8;
        auto is=[&](const char* value){return std::memcmp(tag,value,4)==0;};
        unsigned bit=0;
        if(is("MD21")){bit=1;if(length!=bodySize)return false;}
        else if(is("TXID")){bit=2;if(length%4)return false;}
        else if(is("SFID")){bit=4;if(length%4)return false;}
        else if(is("LDV1")){bit=8;if(length!=16)return false;} // LOD metadata, not particle shading
        else if(is("TXAC")) {
            bit=16;
            if(length!=2*(model.materials.size()+model.particles.size()))return false;
            for(uint32_t n=0;n<length;++n)if(data[n])return false;
        } else if(is("EXP2")) {
            bit=32;
            if(length<8)return false;
            const auto count=u32(data),offset=u32(data+4);
            if(count!=model.particles.size()||offset<8||!fits(offset,count,28,length))return false;
            for(uint32_t n=0;n<count;++n) {
                const auto* record=data+offset+n*28;
                float values[3];std::memcpy(values,record,12);
                if(values[0]!=0||!std::isfinite(values[1])||!std::isfinite(values[2])||
                    values[1]<0||values[1]>16||values[2]<0||values[2]>16)return false;
                const auto times=u32(record+12),timeOffset=u32(record+16),keys=u32(record+20),keyOffset=u32(record+24);
                if(times!=keys||keys>4096||!fits(timeOffset,times,2,length)||!fits(keyOffset,keys,2,length)||
                    (keys&&(!timeOffset||!keyOffset)))return false;
                // Only a constant zero cutoff is verified; animated cutoff stays native.
                uint16_t previous=0;
                for(uint32_t k=0;k<keys;++k) {
                    uint16_t time,value;std::memcpy(&time,data+timeOffset+k*2,2);std::memcpy(&value,data+keyOffset+k*2,2);
                    if(value||time>32767||(k&&time<previous))return false;
                    previous=time;
                }
                model.particleMultipliers[n]={values[1],values[2]};
            }
        } else return false;
        if(seen&bit)return false;
        seen|=bit;at+=8+length;
    }
    return at==size&&(seen&1)!=0;
}
// Limited native-sprite composition experiment. Native simulation owns the global
// 0x3090 model behavior. Only neutral per-emitter metadata is admitted; TXAC on
// another mesh/emitter must not disqualify a neutral sprite. No EXP2/edge/depth
// chunks are interpreted by this path. TXAC 1/1 is captured separately for the
// opt-in material-family approximation; it never enables neutral admission.
inline void CaptureNativeSpriteLayers(const uint8_t* body,uint32_t bodySize,
 const uint8_t* container,uint32_t size,ModelSource& model) {
 model.nativeSpriteLayers.clear();
 model.spriteLayerKinds.clear();
 model.txac11SpriteMask=0;
 // Capture metadata for the guarded Blood Plague path too; generic admission
 // still requires 0x3090 in DecodeNativeSprites.
 if(!body || !container || (model.globalFlags!=0x3090 && model.globalFlags!=0x2090) || model.innerVersion!=274 ||
    model.particleCapture!=ParticleCapture::Captured || model.particles.empty())return;
 auto u32=[](const uint8_t* p){uint32_t v;std::memcpy(&v,p,4);return v;};
 std::vector<uint8_t> eligible(model.particles.size(),1);
 std::vector<uint8_t> kinds(model.particles.size(),1);
 uint32_t at=0,seen=0,txac11=0;
 while(at<=size && size-at>=8) {
  const auto length=u32(container+at+4);if(length>size-at-8)return;
  const auto* tag=container+at;const auto* data=tag+8;unsigned bit=0;
  if(!std::memcmp(tag,"MD21",4)){bit=1;if(length!=bodySize)return;}
  else if(!std::memcmp(tag,"TXID",4)){bit=2;if(length!=4*model.textures.size())return;}
  else if(!std::memcmp(tag,"SFID",4)){bit=4;if(length%4)return;}
  else if(!std::memcmp(tag,"TXAC",4)) {
   bit=8;if(length!=2*(model.materials.size()+model.particles.size()))return;
   for(size_t i=0;i<eligible.size();++i) {
    const auto offset=2*(model.materials.size()+i);
    if(data[offset] || data[offset+1])eligible[i]=0;
    kinds[i]=eligible[i]?1:(data[offset]==1 && data[offset+1]==1?2:0);
    if(i<32 && data[offset]==1 && data[offset+1]==1)txac11|=1u<<i;
   }
  } else return;
  if(seen&bit)return;seen|=bit;at+=8+length;
 }
 if(at!=size || !(seen&1))return;
 for(size_t i=0;i<eligible.size();++i)for(unsigned field:{0x18u,0x20u}) {
  const auto& p=model.particles[i];const auto n=p.U32(field),o=p.U32(field+4);
  if(n && (n!=1 || !o || o>=bodySize || body[o])){eligible[i]=0;kinds[i]=0;if(i<32)txac11&=~(1u<<i);}
 }
 model.nativeSpriteLayers=std::move(eligible);
 model.spriteLayerKinds=std::move(kinds);
 model.txac11SpriteMask=txac11;
 model.particleMultipliers.assign(model.particles.size(),{1.f,1.f});
}

}
