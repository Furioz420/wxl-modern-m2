// Copyright (C) 2026 WarcraftXL. GPL-3.0-or-later.
// Isolated dependency bundle for the audited donor, not a global FDID override.
#pragma once
#include "RiftMeshPolicy.hpp"
#include <array>
#include <span>

namespace wxl::modern::assets::m2::rifttextures {
struct Entry { uint32_t fdid; const char* path; };
#define WXL_RIFT_TEXTURE(id) Entry{id,"spells\\wxl_rift_61621\\" #id ".blp"}
inline constexpr std::array<Entry,27> kEntries{{
    WXL_RIFT_TEXTURE(4696760), WXL_RIFT_TEXTURE(982938), WXL_RIFT_TEXTURE(2458924),
    WXL_RIFT_TEXTURE(4696762), WXL_RIFT_TEXTURE(4696764), WXL_RIFT_TEXTURE(5869143),
    WXL_RIFT_TEXTURE(5737273), WXL_RIFT_TEXTURE(2067352), WXL_RIFT_TEXTURE(4696770),
    WXL_RIFT_TEXTURE(4696792), WXL_RIFT_TEXTURE(4696800), WXL_RIFT_TEXTURE(5041652),
    WXL_RIFT_TEXTURE(5869144), WXL_RIFT_TEXTURE(5869145), WXL_RIFT_TEXTURE(2994790),
    WXL_RIFT_TEXTURE(5869146), WXL_RIFT_TEXTURE(5869147), WXL_RIFT_TEXTURE(4696804),
    WXL_RIFT_TEXTURE(5869148), WXL_RIFT_TEXTURE(4547556), WXL_RIFT_TEXTURE(984062),
    WXL_RIFT_TEXTURE(5869149), WXL_RIFT_TEXTURE(2001608), WXL_RIFT_TEXTURE(4715176),
    WXL_RIFT_TEXTURE(1733523), WXL_RIFT_TEXTURE(5790072), WXL_RIFT_TEXTURE(5869150)
}};
#undef WXL_RIFT_TEXTURE
inline bool Matches(bool enabled,std::string_view name,std::span<const uint8_t> source,
                    std::span<const uint32_t> ids,uint32_t textureCount) noexcept {
    if(!enabled||!riftmesh::Target(name)||source.size()!=613316||
       ids.size()!=kEntries.size()||textureCount!=kEntries.size())return false;
    for(size_t i=0;i<ids.size();++i)if(ids[i]!=kEntries[i].fdid)return false;
    // Non-cryptographic identity guard plus exact ordered IDs/size/path; not a trust boundary.
    uint64_t hash=14695981039346656037ull;
    for(auto byte:source){hash^=byte;hash*=1099511628211ull;}
    return hash==0xed4dfce6eaa818ebull;
}
inline bool CanBind(std::span<const wxl::structure::m2::M2Texture> textures) noexcept {
    if(textures.size()!=kEntries.size())return false;
    for(const auto& texture:textures)
        if(texture.type!=0||texture.filename.count>=2)return false;
    return true;
}
}
