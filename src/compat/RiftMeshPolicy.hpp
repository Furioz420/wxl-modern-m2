// Copyright (C) 2026 WarcraftXL. GPL-3.0-or-later.
// Test-32 capability slice: named effect 33 -> native two-UV Mod_Mod2x.
// This does NOT implement its edge-fade vertex stage, depth effects or particles.
#pragma once
#include "SourceMaterialCapture.hpp"
#include <string_view>

namespace wxl::modern::assets::m2::riftmesh {
inline char Fold(char c) noexcept {
    if(c=='/')return '\\';
    return c>='A'&&c<='Z'?char(c-'A'+'a'):c;
}
inline bool Target(std::string_view path) noexcept {
    constexpr std::string_view stem="spells\\cfx_priest_entropicrift_areatrigger";
    if(path.size()!=stem.size()&&path.size()!=stem.size()+3&&path.size()!=stem.size()+4)return false;
    for(size_t i=0;i<stem.size();++i)if(Fold(path[i])!=stem[i])return false;
    if(path.size()==stem.size())return true; // Skin finalize supplies a stem.
    if(path.size()==stem.size()+3)return Fold(path[stem.size()])=='.'&&Fold(path[stem.size()+1])=='m'&&path.back()=='2';
    return path[stem.size()]=='.'&&Fold(path[stem.size()+1])=='m'&&Fold(path[stem.size()+2])=='d'&&Fold(path.back())=='x';
}
inline bool Eligible(bool enabled,std::string_view name,const material::ModelSource* model,
                     const wxl::structure::m2::M2Batch& batch) noexcept {
    if(!enabled||!Target(name)||!model||model->containerMagic!=wxl::structure::m2::kMagicMD21||
       model->innerVersion!=274||batch.shaderId!=0x8021||batch.textureCount!=2||
       batch.materialIndex>=model->materials.size())return false;
    const auto& material=model->materials[batch.materialIndex];
    if(!(material.flags&1)||(material.blend!=2&&material.blend!=7))return false;
    const size_t combo=batch.textureComboIndex;
    if(combo>=model->textureCombos.size()||model->textureCombos.size()-combo<2)return false;
    for(size_t i=0;i<2;++i){
        const auto texture=model->textureCombos[combo+i];
        if(texture>=model->textures.size()||model->textures[texture].type!=0)return false;
    }
    return true;
}
inline constexpr uint16_t kPackedModMod2x=0x4014;
static_assert(material::Classify(material::Profile::Legion26365,0x8021,2).pixel==
              material::Classify(material::Profile::Legion26365,kPackedModMod2x,2).pixel);
}
