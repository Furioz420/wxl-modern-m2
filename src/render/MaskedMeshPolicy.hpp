#pragma once
#include "../compat/SourceMaterialCapture.hpp"
namespace wxl::modern::materialblend {
// Experimental metadata family derived from the accepted three-map DnD trial.
// GPU admission must ALSO match the captured VS/PS and blend/depth contract.
inline bool MaskedMeshEligible(bool enabled,const assets::m2::material::SkinSource& s,
    unsigned index,const wxl::structure::m2::M2Batch& live,unsigned blend,uint16_t& maskIndex) noexcept {
    namespace mat=assets::m2::material;
    if(!enabled||!s.valid||!s.model||index>=s.outputs.size()||blend!=4)return false;
    const auto& m=*s.model;const auto& origin=s.outputs[index];
    if(m.containerMagic!=wxl::structure::m2::kMagicMD21||m.innerVersion!=274||m.globalFlags!=0x3090||
       origin.parked||origin.split||origin.piece||origin.sourceBatch>=s.batches.size())return false;
    const auto& raw=s.batches[origin.sourceBatch];
    if(raw.shaderId!=0x4013||raw.textureCount!=3||(raw.flags&0xff)!=0x8c||raw.materialIndex>=m.materials.size()||
       m.materials[raw.materialIndex].flags!=0x13||m.materials[raw.materialIndex].blend!=7||
       m.meshTxacState!=mat::MeshTxacState::Captured||raw.materialIndex>=m.meshTxac.size()||
       m.meshTxac[raw.materialIndex]!=std::array<uint8_t,2>{1,1}||
       live.shaderId!=raw.shaderId||live.textureCount!=2||live.materialIndex!=raw.materialIndex||
       live.colorIndex!=raw.colorIndex||live.textureComboIndex!=raw.textureComboIndex||
       live.textureWeightComboIndex!=raw.textureWeightComboIndex||
       live.textureTransformComboIndex!=raw.textureTransformComboIndex)return false;
    for(unsigned j=0;j<3;++j) {
        const unsigned at=unsigned(raw.textureComboIndex)+j;
        if(at>=m.textureCombos.size())return false;
        const auto t=m.textureCombos[at];
        if(t>=m.textures.size()||t>=m.textureFileDataIds.size()||!m.textureFileDataIds[t]||
           m.textures[t].type||m.textures[t].flags!=(j==2?4u:3u))return false;
    }
    const unsigned transform=unsigned(raw.textureTransformComboIndex)+2;
    if(transform>=m.transformCombos.size()||m.transformCombos[transform]!=0xffff)return false;
    for(unsigned j=0;j<2;++j)
        if(m.transformCombos[unsigned(raw.textureTransformComboIndex)+j]>=m.sourceTransformCount)return false;
    const unsigned weight=unsigned(raw.textureWeightComboIndex)+2;
    if(weight<m.weightCombos.size()&&m.weightCombos[weight]!=0xffff)return false;
    maskIndex=m.textureCombos[unsigned(raw.textureComboIndex)+2];return true;
}
}
