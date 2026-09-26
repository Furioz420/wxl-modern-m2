// Copyright (C) 2026 WarcraftXL. GPL-3.0-or-later.
#pragma once
#include <string_view>
namespace wxl::modern::particlelayers {
    // Diagnostic selector only. Never used by production source eligibility.
    inline bool ViewPathMatches(std::string_view path,std::string_view wanted) noexcept {
        if(wanted.empty()||path.size()!=wanted.size())return false;
        auto fold=[](char c){if(c=='/')return '\\';return c>='A'&&c<='Z'?char(c+('a'-'A')):c;};
        for(size_t n=0;n<path.size();++n)if(fold(path[n])!=fold(wanted[n]))return false;
        return true;
    }
    inline bool ViewTarget(bool enabled,std::string_view path,std::string_view wanted,
                           unsigned meshVertices,unsigned emitters,unsigned selected) noexcept {
        return enabled&&!meshVertices&&selected<emitters&&ViewPathMatches(path,wanted);
    }
    inline bool HideAllDraw(bool enabled,bool target) noexcept {return enabled&&target;}
    inline bool ValidView(unsigned view) noexcept {return view<=4;}
    inline bool HideEmitter(bool target,unsigned selected,unsigned emitter) noexcept {return target&&selected!=emitter;}
}
