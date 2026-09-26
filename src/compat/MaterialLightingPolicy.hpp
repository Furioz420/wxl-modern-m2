// Copyright (C) 2026 WarcraftXL. GPL-3.0-or-later.
#pragma once
#include "SourceMaterialCapture.hpp"
namespace wxl::modern::assets::m2::material {
// Pre-native-finalize policy. Only reverses the translator-added UNLIT bit; does
// not restore arbitrary flags or claim that native Add equals modern BlendAdd.
inline bool RestoreAuthoredLighting(bool enabled,const SkinSource* source,unsigned skinProfiles,
    const wxl::structure::m2::M2Batch* live,unsigned count,unsigned materialIndex,
    uint16_t liveFlags,uint16_t liveBlend,const uint16_t* combiners,unsigned combinerCount,
    const int16_t* coords,unsigned coordCount,uint16_t& out) noexcept {
    if(!enabled||!source||!source->valid||!source->model||skinProfiles!=1||!live||!count||count>1024||
       source->outputs.size()!=count||source->batches.empty()||source->batches.size()>1024||
       !combiners||!coords||combinerCount>65536||coordCount>65536||liveBlend!=4)return false;
    const auto& model=*source->model;
    if(model.containerMagic!=wxl::structure::m2::kMagicMD21||model.innerVersion!=274||
       model.materials.size()>256||materialIndex>=model.materials.size())return false;
    const auto rawMaterial=model.materials[materialIndex];
    if(rawMaterial.blend!=7||(rawMaterial.flags&~0x1fu)||(rawMaterial.flags&1u)||
       liveFlags!=uint16_t(rawMaterial.flags|5u))return false;
    unsigned users=0;
    // Every original user must map to exactly one ordinary, unparked, unsplit output.
    // Material records are shared: never change one that also feeds another family.
    for(unsigned rawIndex=0;rawIndex<source->batches.size();++rawIndex) {
        const auto& raw=source->batches[rawIndex];if(raw.materialIndex!=materialIndex)continue;
        if(raw.shaderId!=0x4014||raw.textureCount!=2)return false;
        unsigned matches=0;
        for(unsigned i=0;i<count;++i) {
            const auto& origin=source->outputs[i];if(origin.sourceBatch!=rawIndex)continue;
            const auto& b=live[i];
            if(origin.parked||origin.piece||origin.split||b.materialIndex!=materialIndex||b.textureCount!=2||
               b.colorIndex!=raw.colorIndex||b.textureComboIndex!=raw.textureComboIndex||
               b.textureWeightComboIndex!=raw.textureWeightComboIndex||b.textureTransformComboIndex!=raw.textureTransformComboIndex||
               b.materialLayer!=raw.materialLayer||unsigned(b.shaderId)+1>=combinerCount||
               combiners[b.shaderId]!=1||combiners[b.shaderId+1]!=4||
               unsigned(b.textureCoordComboIndex)+1>=coordCount||coords[b.textureCoordComboIndex]!=0||
               coords[b.textureCoordComboIndex+1]!=1)return false;
            ++matches;
        }
        if(matches!=1)return false;
        ++users;
    }
    // Reject foreign/unmapped live users of the shared material as well.
    unsigned liveUsers=0;for(unsigned i=0;i<count;++i)if(live[i].materialIndex==materialIndex)++liveUsers;
    if(!users||users!=liveUsers)return false;
    out=uint16_t(liveFlags&~1u);return true;
}
}
