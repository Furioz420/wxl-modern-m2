// Copyright (C) 2026 WarcraftXL. GPL-3.0-or-later.
#pragma once
#include "../ExtensionApi.hpp"

namespace wxl::modern::materialconfig {
// Profile 1 is the tested guarded material subset, not a source version or an
// assertion that all modern M2 shaders are supported. No diagnostic is enabled.
inline unsigned ParseMode(const char* text, unsigned maximum) noexcept {
    return text && text[0]>='0' && text[0]<='9' && !text[1] &&
        unsigned(text[0]-'0')<=maximum ? unsigned(text[0]-'0') : 0;
}
inline bool RawMode(const char* key,char* value,size_t capacity) {
    // Preserve explicit invalid/oversized environment values as OFF, rather than
    // falling through to the cfg/preset. Config values fit its 512-byte line cap.
    const DWORD length=GetEnvironmentVariableA(key,value,DWORD(capacity));
    if(length) { if(length>=capacity)value[0]=0; return true; }
    return wxl_modern_m2::ConfigRaw(key,value,capacity);
}
inline unsigned Profile() {
    char value[512]{};
    return RawMode("WXL_M2_MATERIAL_PROFILE",value,sizeof(value))?ParseMode(value,1):0;
}
inline bool Feature(const char* key) {
    // Existing per-feature environment/cfg semantics take precedence; profile is
    // only their missing-value default. Explicit 0 continues to disable a feature.
    const bool member=key&&(!std::strcmp(key,"WXL_M2_PARTICLE_LAYERS")||!std::strcmp(key,"WXL_M2_PARTICLE_MATERIAL")||
        !std::strcmp(key,"WXL_M2_PARTICLE_MODULATION")||!std::strcmp(key,"WXL_M2_BLEND7_EXPERIMENT")||
        !std::strcmp(key,"WXL_M2_BLEND7_SOURCE_LIGHTING"));
    return wxl_modern_m2::ConfigBool(key,member&&Profile()==1);
}
inline unsigned RibbonMode() {
    char value[512]{};
    return RawMode("WXL_M2_RIBBON_SHADER",value,sizeof(value))?ParseMode(value,3):(Profile()==1?3u:0u);
}
}
