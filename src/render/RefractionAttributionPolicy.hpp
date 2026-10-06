// Copyright (C) 2026 WarcraftXL. GPL-3.0-or-later.
#pragma once
#include "ParticleViewPolicy.hpp"
#include "../compat/SourceMaterialCapture.hpp"

namespace wxl::modern::particlelayers {
// Exact captured signature used by the omission diagnostic and the independent
// opt-in refraction approximation. Does not establish general source support.
inline bool MatchBloodBoilRefraction(bool enabled, std::string_view path,
    const assets::m2::material::ModelSource& model, unsigned emitter) noexcept {
    if (!enabled || !ViewPathMatches(path, "spells\\cfx_deathknight_bloodboil_castworld.m2") ||
        model.containerMagic != wxl::structure::m2::kMagicMD21 ||
        model.innerVersion != 274 || model.globalFlags != 0x3090 ||
        model.particleCapture != assets::m2::material::ParticleCapture::Captured ||
        model.particles.size() != 7 || model.textures.size() != 6 ||
        model.textureFileDataIds.size() != 6 || emitter != 0) return false;
    const auto& p = model.particles[0];
    return p.Flags() == 0x60931011 && p.Blend() == 4 &&
        p.U16(0x16) == 0 && model.textureFileDataIds[0] == 1601218;
}
inline bool OmitBloodBoilRefraction(bool enabled,std::string_view path,
    const assets::m2::material::ModelSource& model,unsigned emitter) noexcept {
    return MatchBloodBoilRefraction(enabled,path,model,emitter);
}
}
