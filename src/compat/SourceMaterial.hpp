// Copyright (C) 2026 WarcraftXL. GPL-3.0-or-later.
// Pure source-material classification; intentionally not connected to live rendering.
// See docs/modern-m2-material-classifier-2026-09-05.md for evidence and integration gates.
#pragma once

#include <array>
#include <cstdint>

namespace wxl::modern::assets::m2::material
{
    // Must be selected from known source provenance, never just the inner M2 version.
    enum class Profile : uint8_t { Unknown, NativeWotlk, Legion26365 };
    enum class Disposition : uint8_t { NativePassthrough, Classified, Unclassified };
    enum class Evidence : uint8_t { None, NativeContract, DocumentedPackedRules, RecoveredLegionTable };
    enum class Reason : uint8_t
    {
        None, UnknownProfile, InvalidTextureCount, PackedTextureCountUnsupported, NamedEffectOutOfRange
    };

    // Values are the recovered Legion FAMILY array indices, not source effect IDs.
    enum class Pixel : uint8_t
    {
        Opaque, Mod, OpaqueMod, OpaqueMod2x, OpaqueMod2xNA, OpaqueOpaque,
        ModMod, ModMod2x, ModAdd, ModMod2xNA, ModAddNA, ModOpaque,
        OpaqueMod2xNAAlpha, OpaqueAddAlpha, OpaqueAddAlphaAlpha,
        OpaqueMod2xNAAlphaAdd, ModAddAlpha, ModAddAlphaAlpha, OpaqueAlphaAlpha,
        OpaqueMod2xNAAlpha3s, OpaqueAddAlphaWgt, ModAddAlphaComplement,
        OpaqueModNAAlpha, ModAddAlphaWgt, OpaqueModAddWgt,
        OpaqueMod2xNAAlphaUnshAlpha, ModDualCrossfade, OpaqueMod2xNAAlphaAlpha,
        ModMaskedDualCrossfade, OpaqueAlpha, Guild, GuildNoBorder, GuildOpaque,
        ModDepth, Illum, ModModModConst, Unknown = 255
    };

    enum class Vertex : uint8_t
    {
        DiffuseT1, DiffuseEnv, DiffuseT1T2, DiffuseT1Env, DiffuseEnvT1,
        DiffuseEnvEnv, DiffuseT1EnvT1, DiffuseT1T1, DiffuseT1T1T1,
        DiffuseEdgeFadeT1, DiffuseT2, DiffuseT1EnvT2, DiffuseEdgeFadeT1T2,
        DiffuseEdgeFadeEnv, DiffuseT1T2T1, DiffuseT1T2T3, ColorT1T2T3,
        BwDiffuseT1, BwDiffuseT1T2, Unknown = 255
    };

    struct Effect
    {
        Pixel pixel;
        Vertex vertex;
        uint8_t hull;
        uint8_t domain;
    };

    // Identical records in the supplied PE32 and PE32+ Legion clients.
    // Original binary hashes and the independently stored CSV are in outer WXL docs/research.
    inline constexpr std::array<Effect, 34> kLegion26365Effects{{
        {Pixel::OpaqueMod2xNAAlpha, Vertex::DiffuseT1Env, 1, 1},
        {Pixel::OpaqueAddAlpha, Vertex::DiffuseT1Env, 1, 1},
        {Pixel::OpaqueAddAlphaAlpha, Vertex::DiffuseT1Env, 1, 1},
        {Pixel::OpaqueMod2xNAAlphaAdd, Vertex::DiffuseT1EnvT1, 2, 2},
        {Pixel::ModAddAlpha, Vertex::DiffuseT1Env, 1, 1},
        {Pixel::OpaqueAddAlpha, Vertex::DiffuseT1T1, 1, 1},
        {Pixel::ModAddAlpha, Vertex::DiffuseT1T1, 1, 1},
        {Pixel::ModAddAlphaAlpha, Vertex::DiffuseT1Env, 1, 1},
        {Pixel::OpaqueAlphaAlpha, Vertex::DiffuseT1Env, 1, 1},
        {Pixel::OpaqueMod2xNAAlpha3s, Vertex::DiffuseT1EnvT1, 2, 2},
        {Pixel::OpaqueAddAlphaWgt, Vertex::DiffuseT1T1, 1, 1},
        {Pixel::ModAddAlphaComplement, Vertex::DiffuseT1Env, 1, 1},
        {Pixel::OpaqueModNAAlpha, Vertex::DiffuseT1Env, 1, 1},
        {Pixel::ModAddAlphaWgt, Vertex::DiffuseT1Env, 1, 1},
        {Pixel::ModAddAlphaWgt, Vertex::DiffuseT1T1, 1, 1},
        {Pixel::OpaqueAddAlphaWgt, Vertex::DiffuseT1T2, 1, 1},
        {Pixel::OpaqueModAddWgt, Vertex::DiffuseT1Env, 1, 1},
        {Pixel::OpaqueMod2xNAAlphaUnshAlpha, Vertex::DiffuseT1EnvT1, 2, 2},
        {Pixel::ModDualCrossfade, Vertex::DiffuseT1, 0, 0},
        {Pixel::ModDepth, Vertex::DiffuseEdgeFadeT1, 0, 0},
        {Pixel::OpaqueMod2xNAAlphaAlpha, Vertex::DiffuseT1EnvT2, 2, 2},
        {Pixel::ModMod, Vertex::DiffuseEdgeFadeT1T2, 1, 1},
        {Pixel::ModMaskedDualCrossfade, Vertex::DiffuseT1T2, 1, 1},
        {Pixel::OpaqueAlpha, Vertex::DiffuseT1T1, 1, 1},
        {Pixel::OpaqueMod2xNAAlphaUnshAlpha, Vertex::DiffuseT1EnvT2, 2, 2},
        {Pixel::ModDepth, Vertex::DiffuseEdgeFadeEnv, 0, 0},
        {Pixel::Guild, Vertex::DiffuseT1T2T1, 2, 1},
        {Pixel::GuildNoBorder, Vertex::DiffuseT1T2, 1, 2},
        {Pixel::GuildOpaque, Vertex::DiffuseT1T2T1, 2, 1},
        {Pixel::Illum, Vertex::DiffuseT1T1, 1, 1},
        {Pixel::ModModModConst, Vertex::DiffuseT1T2T3, 2, 2},
        {Pixel::ModModModConst, Vertex::ColorT1T2T3, 2, 2},
        {Pixel::Opaque, Vertex::DiffuseT1, 0, 0},
        {Pixel::ModMod2x, Vertex::DiffuseEdgeFadeT1T2, 1, 1},
    }};

    struct Classification
    {
        Profile profile = Profile::Unknown;
        uint16_t sourceShaderId = 0;
        uint16_t textureCount = 0;
        Disposition disposition = Disposition::Unclassified;
        Evidence evidence = Evidence::None;
        Reason reason = Reason::None;
        bool namedEffect = false;
        uint16_t effectIndex = 0xffff;
        Pixel pixel = Pixel::Unknown;
        Vertex vertex = Vertex::Unknown;
        uint8_t hullIndex = 0xff;
        uint8_t domainIndex = 0xff;
    };

    constexpr bool HasEdgeFade(Vertex vertex) noexcept
    {
        return vertex == Vertex::DiffuseEdgeFadeT1 || vertex == Vertex::DiffuseEdgeFadeT1T2 ||
               vertex == Vertex::DiffuseEdgeFadeEnv;
    }

    constexpr const char* ReasonName(Reason reason) noexcept
    {
        switch (reason)
        {
        case Reason::None: return "none";
        case Reason::UnknownProfile: return "unknown-source-profile";
        case Reason::InvalidTextureCount: return "invalid-texture-count";
        case Reason::PackedTextureCountUnsupported: return "packed-stage-count-outside-reference";
        case Reason::NamedEffectOutOfRange: return "named-effect-outside-source-table";
        }
        return "unknown-reason";
    }

    // Classified means selection metadata is known, NOT that a renderer supports it.
    // No file access, allocation, shader creation, mutation or native state is involved.
    constexpr Classification Classify(Profile profile, uint16_t shaderId, uint16_t textureCount) noexcept
    {
        Classification result{};
        result.profile = profile;
        result.sourceShaderId = shaderId;
        result.textureCount = textureCount;
        if (profile == Profile::NativeWotlk)
        {
            // Native fields are for the native client to interpret, including sentinel values.
            result.disposition = Disposition::NativePassthrough;
            result.evidence = Evidence::NativeContract;
            return result;
        }
        if (profile != Profile::Legion26365)
        {
            result.reason = Reason::UnknownProfile;
            return result;
        }
        if (textureCount == 0 || textureCount > 4)
        {
            result.reason = Reason::InvalidTextureCount;
            return result;
        }
        if ((shaderId & 0x8000u) != 0)
        {
            result.namedEffect = true;
            result.effectIndex = shaderId & 0x7fffu;
            if (result.effectIndex >= kLegion26365Effects.size())
            {
                result.reason = Reason::NamedEffectOutOfRange;
                return result;
            }
            const Effect effect = kLegion26365Effects[result.effectIndex];
            result.pixel = effect.pixel;
            result.vertex = effect.vertex;
            result.hullIndex = effect.hull;
            result.domainIndex = effect.domain;
            result.evidence = Evidence::RecoveredLegionTable;
        }
        else
        {
            if (textureCount > 2)
            {
                result.reason = Reason::PackedTextureCountUnsupported;
                return result;
            }
            result.evidence = Evidence::DocumentedPackedRules;
            if (textureCount == 1)
            {
                result.pixel = (shaderId & 0x70u) ? Pixel::Mod : Pixel::Opaque;
                result.vertex = (shaderId & 0x80u) ? Vertex::DiffuseEnv :
                                (shaderId & 0x4000u) ? Vertex::DiffuseT2 : Vertex::DiffuseT1;
            }
            else
            {
                constexpr Pixel mod[8] = {Pixel::ModOpaque, Pixel::ModMod, Pixel::ModMod,
                    Pixel::ModAdd, Pixel::ModMod2x, Pixel::ModMod, Pixel::ModMod2xNA, Pixel::ModAddNA};
                constexpr Pixel opaque[8] = {Pixel::OpaqueOpaque, Pixel::OpaqueMod, Pixel::OpaqueMod,
                    Pixel::OpaqueAddAlpha, Pixel::OpaqueMod2x, Pixel::OpaqueMod,
                    Pixel::OpaqueMod2xNA, Pixel::OpaqueAddAlpha};
                result.pixel = (shaderId & 0x70u) ? mod[shaderId & 7u] : opaque[shaderId & 7u];
                if (shaderId & 0x80u)
                    result.vertex = (shaderId & 8u) ? Vertex::DiffuseEnvEnv : Vertex::DiffuseEnvT1;
                else
                    result.vertex = (shaderId & 8u) ? Vertex::DiffuseT1Env :
                                    (shaderId & 0x4000u) ? Vertex::DiffuseT1T2 : Vertex::DiffuseT1T1;
            }
        }
        result.disposition = Disposition::Classified;
        return result;
    }
}
