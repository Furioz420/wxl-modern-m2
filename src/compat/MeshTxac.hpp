#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <vector>
namespace wxl::modern::assets::m2::material {
enum class MeshTxacState : uint8_t { Unknown, Absent, Captured, Invalid };
// Owned per-material bytes, not a shader-selection rule. Never borrow chunk memory.
inline MeshTxacState ReadMeshTxac(const uint8_t* data,uint32_t size,
    uint32_t materials,uint32_t particles,std::vector<std::array<uint8_t,2>>& out) {
    out.clear();
    if(!data || !size)return MeshTxacState::Unknown;
    if(materials>65536 || particles>65536)return MeshTxacState::Invalid;
    const uint8_t* txac=nullptr;uint32_t txacSize=0;bool model=false;
    for(uint32_t at=0;at<size;) {
        if(size-at<8)return MeshTxacState::Invalid;
        uint32_t length=0;std::memcpy(&length,data+at+4,4);
        if(length>size-at-8)return MeshTxacState::Invalid;
        if(!std::memcmp(data+at,"MD21",4)) {
            if(model)return MeshTxacState::Invalid;
            model=true;
        }
        if(!std::memcmp(data+at,"TXAC",4)) {
            if(txac)return MeshTxacState::Invalid;
            txac=data+at+8;txacSize=length;
        }
        at+=8+length;
    }
    if(!model)return MeshTxacState::Invalid;
    if(!txac)return MeshTxacState::Absent;
    if(txacSize!=2*(materials+particles))return MeshTxacState::Invalid;
    out.resize(materials);
    for(uint32_t n=0;n<materials;++n)out[n]={txac[2*n],txac[2*n+1]};
    return MeshTxacState::Captured;
}
}
