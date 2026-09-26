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
}
