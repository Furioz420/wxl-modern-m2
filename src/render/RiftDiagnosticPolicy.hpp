// Read-only diagnostics. No rendering eligibility or byte mutation. GPLv3.
#pragma once
#include "ParticleLayerPolicy.hpp"
#include "ParticleViewPolicy.hpp"
#include <atomic>
#include <span>
namespace wxl::modern::riftdiag {
inline std::atomic<bool> enabled{false};
inline bool Target(std::string_view path) noexcept {
    return particlelayers::ViewPathMatches(path,"spells\\cfx_priest_entropicrift_areatrigger.m2") ||
           particlelayers::ViewPathMatches(path,"spells\\cfx_priest_entropicrift_areatrigger.mdx");
}
// Describes independent coarse gates; Decode remains the authoritative full predicate.
inline uint32_t Blockers(const assets::m2::material::ModelSource& m,unsigned i) noexcept {
    namespace mat=assets::m2::material;
    uint32_t bits=0;
    if(m.containerMagic!=wxl::structure::m2::kMagicMD21 || m.innerVersion!=274)bits|=1;
    if(!m.particleLayerFeaturesKnown)bits|=2;
    if(m.globalFlags&0x1000)bits|=4;
    if(m.particleCapture!=mat::ParticleCapture::Captured || i>=m.particles.size())return bits|8;
    const auto& p=m.particles[i];
    if(!p.MultiTexture() || p.Blend()!=7)bits|=16;
    if(p.Flags()&0x80100000u)bits|=32;
    if(i>=m.particleMultipliers.size())bits|=64;
    for(unsigned s=0;s<3;++s)if(p.Texture(s)>=m.textures.size() || m.textures[p.Texture(s)].type!=0)bits|=128;
    if((p.bytes[0x2c]|p.bytes[0x2d])&0x80)bits|=256;
    return bits;
}
inline uint64_t Fingerprint(std::span<const uint8_t> bytes) noexcept {
    uint64_t value=14695981039346656037ull;
    for(auto b:bytes){value^=b;value*=1099511628211ull;}
    return value;
}
}
