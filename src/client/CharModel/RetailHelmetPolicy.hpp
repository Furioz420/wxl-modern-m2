#pragma once
#include <string>
#include <algorithm>
#include <cstdint>
namespace WXL {
inline std::string EquipmentPathKey(const char* raw) {
    std::string s=raw?raw:"";
    for(char& c:s) { if(c=='/')c='\\';if(c>='A'&&c<='Z')c=char(c-'A'+'a'); }
    return s;
}
inline bool RetailHelmetActor(const char* model,uint32_t race,bool retailAllowed,bool nativeModern=false) {
    if(!retailAllowed || race==16 || race==23 || !race)return false;
    auto path=EquipmentPathKey(model);
    if(path.find("eredar")!=std::string::npos || path.find("broken")!=std::string::npos)return false;
    // Storage redirection preserves the requested alias on some NPC models.
    // A modern-loaded actor may therefore lack the _hd suffix; actual legacy bodies
    // must still retain their original helmet family.
    if(path.starts_with("hd\\"))path.erase(0,3);
    if(race<=11 && !nativeModern && path.find("_hd.m2")==std::string::npos)return false;
    return path.starts_with("character\\") || path.starts_with("creature\\");
}
inline std::string RetailHelmetAlias(const char* raw) {
    auto path=EquipmentPathKey(raw);
    const std::string prefix="item\\objectcomponents\\head\\";
    if(!path.starts_with(prefix))return {};
    auto name=path.substr(prefix.size());
    if(name.empty()||name.find('\\')!=std::string::npos||name.find("..")!=std::string::npos)return {};
    if(name.ends_with(".mdx"))name.replace(name.size()-4,4,".m2");
    if(!name.ends_with(".m2"))return {};
    return prefix+"wxl_retail\\"+name;
}
}
