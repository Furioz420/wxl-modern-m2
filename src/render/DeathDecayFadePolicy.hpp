#pragma once
#include "../compat/SourceMaterialCapture.hpp"
#include "ParticleViewPolicy.hpp"
namespace wxl::modern::materialblend {
inline bool DeathDecayFadeEligible(bool enabled,std::string_view path,
    const assets::m2::material::SkinSource& s,unsigned index,
    const wxl::structure::m2::M2Batch& live,unsigned blend) noexcept {
    if(!enabled || !particlelayers::ViewPathMatches(path,"spells\\cfx_deathknight_deathanddecay_state.m2") ||
       !s.valid || !s.model || index>=s.outputs.size() || blend!=4)return false;
    const auto& m=*s.model;const auto& origin=s.outputs[index];
    if(m.containerMagic!=wxl::structure::m2::kMagicMD21 || m.innerVersion!=274 || m.globalFlags!=0x3090 ||
       m.textures.size()!=18 || m.textureFileDataIds.size()!=18 || s.batches.size()!=3 ||
       origin.parked || origin.piece || origin.split || origin.sourceBatch>1)return false;
    const auto& raw=s.batches[origin.sourceBatch];
    if(raw.shaderId!=0x4013 || raw.textureCount!=3 || raw.materialIndex>=m.materials.size() ||
       m.materials[raw.materialIndex].flags!=0x13 || m.materials[raw.materialIndex].blend!=7 ||
       live.shaderId!=0x4013 || live.textureCount!=2 || live.materialIndex!=raw.materialIndex ||
       live.textureComboIndex!=raw.textureComboIndex || live.colorIndex!=raw.colorIndex ||
       live.textureWeightComboIndex!=raw.textureWeightComboIndex ||
       live.textureTransformComboIndex!=raw.textureTransformComboIndex)return false;
    const unsigned expected[2][3]={{1603902,1522978,1603903},{1601200,1620780,1612964}};
    for(unsigned j=0;j<3;++j){
        const unsigned at=unsigned(raw.textureComboIndex)+j;
        if(at>=m.textureCombos.size())return false;
        const auto t=m.textureCombos[at];
        if(t>=18 || m.textures[t].type || m.textureFileDataIds[t]!=expected[origin.sourceBatch][j])return false;
    }
    return true;
}
inline float DeathDecayCombinedAlpha(float fade,float first,float second) noexcept {
    return fade*(first+second);
}
}
