// Copyright (C) 2026 WarcraftXL. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "extension-support/environment/RenderSettings.hpp"
namespace wxl::modern::shadowsettings {
    namespace settings=wxl::render::settings;
    inline constexpr settings::Field fields[]{{"enhanced",0,1,true}};
    inline settings::Result Load(const std::filesystem::path& file,bool& enhanced) {
        std::array<float,1> value{}; const auto result=settings::Load(file,fields,value);
        if(result==settings::Result::Ok) enhanced=value[0]!=0;
        return result;
    }
    inline settings::Result Save(const std::filesystem::path& file,bool enhanced) {
        return settings::Save(file,fields,std::array<float,1>{enhanced?1.f:0.f});
    }
    struct Tuning { bool enhanced=true; float strength=1.35f,softness=1.35f; };
    inline constexpr settings::Field tuningFields[]{{"enhanced",0,1,true},{"strength",.5f,2},{"softness",1,2}};
    inline settings::Result LoadPreferred(const std::filesystem::path& file,const std::filesystem::path& legacy,Tuning& tuning) {
        std::array<float,3> values{};
        auto result=settings::Load(file,tuningFields,values);
        if(result==settings::Result::Ok) tuning={values[0]!=0,values[1],values[2]};
        else if(result==settings::Result::Missing) {
            bool enabled=tuning.enhanced; result=Load(legacy,enabled);
            if(result==settings::Result::Ok) tuning={enabled,1.35f,1.35f};
        }
        return result;
    }
    inline settings::Result Save(const std::filesystem::path& file,const Tuning& tuning) {
        return settings::Save(file,tuningFields,std::array<float,3>{tuning.enhanced?1.f:0.f,tuning.strength,tuning.softness});
    }
}
