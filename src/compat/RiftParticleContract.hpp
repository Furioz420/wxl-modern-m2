// Exact-source native-motion compatibility experiment, not general retail admission. GPLv3.
#pragma once
#include "SourceMaterialCapture.hpp"
#include <cstring>

namespace wxl::modern::assets::m2::material {
// Six sprite emitters have zero TXAC, zero EXP2 cutoff, unity multipliers and
// geoset zero. Other emitters, EDGF/DBOC and global flag 0x1000 are NOT declared
// supported. This profile deliberately retains the already-converted native
// simulation/geometry and substitutes only their three-map draw composition.
inline constexpr uint32_t kRiftNativeSpriteMask = 0x21f; // 0..4 and 9
inline uint32_t CaptureRiftNativeSpriteContract(const uint8_t* data, uint32_t size,
                                               const ModelSource& model) noexcept {
    if(!data || size!=613316 || model.containerMagic!=wxl::structure::m2::kMagicMD21 ||
       model.innerVersion!=274 || model.globalFlags!=0x203090 ||
       model.particleCapture!=ParticleCapture::Captured || model.particles.size()!=13 ||
       model.materials.size()!=11 || model.textures.size()!=27 ||
       model.particleMultipliers.size()!=13)return 0;
    uint64_t hash=14695981039346656037ull;
    for(uint32_t i=0;i<size;++i){hash^=data[i];hash*=1099511628211ull;}
    if(hash!=0xed4dfce6eaa818ebull)return 0;
    // Validate selected emitter properties against retained metadata as well as
    // full original bytes. No partially accepted feature boolean is promoted.
    for(unsigned i=0;i<13;++i)if(kRiftNativeSpriteMask&(1u<<i)) {
        const auto& p=model.particles[i];
        if(!p.MultiTexture() || p.Blend()!=7 || (p.Flags()&0x80100000u) ||
           p.U32(0x18) || p.U32(0x20) ||
           model.particleMultipliers[i][0]!=1 || model.particleMultipliers[i][1]!=1)return 0;
        for(unsigned s=0;s<3;++s)
            if(p.Texture(s)>=model.textures.size() || model.textures[p.Texture(s)].type!=0)return 0;
    }
    return kRiftNativeSpriteMask;
}
}
