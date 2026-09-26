// Copyright (C) 2026 WarcraftXL. GPL-3.0-or-later.
#pragma once
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace wxl::modern::particlematerial
{
    // Explicit backend choices, NOT raw M2 flags or particle blend numbers.
    enum class Combine : uint8_t { TwoColorThreeAlpha, ThreeColorThreeAlpha, Unsupported };
    enum class Lighting : uint8_t { Unlit, Directional, DirectionalPoint, DirectionalTwoPoints, Unsupported };
    struct Vec2 { float x, y; };
    struct Vec3 { float x, y, z; };
    struct Color { float r, g, b, a; };
    // Native prefix retained; independently evaluated extra UVs appended, never aliases of UV0.
    struct UnlitVertex { Vec3 position; uint32_t color; Vec2 uv0, uv1, uv2; };
    struct LitVertex { Vec3 position, normal; uint32_t color; Vec2 uv0, uv1, uv2; };
    static_assert(sizeof(UnlitVertex)==40 && offsetof(UnlitVertex,uv0)==16 &&
        offsetof(UnlitVertex,uv1)==24 && offsetof(UnlitVertex,uv2)==32);
    static_assert(sizeof(LitVertex)==52 && offsetof(LitVertex,normal)==12 &&
        offsetof(LitVertex,color)==24 && offsetof(LitVertex,uv0)==28 &&
        offsetof(LitVertex,uv1)==36 && offsetof(LitVertex,uv2)==44);

    struct Readiness
    {
        uint32_t innerVersion = 0;
        bool modernOwner = false, decodedVariant = false, independentUVs = false;
        bool extendedVertexABI = false, blendResolved = false, uniformsReady = false;
        Combine combine = Combine::Unsupported;
        Lighting lighting = Lighting::Unsupported;
        std::array<uintptr_t,3> textures{};
    };
    inline bool CanDispatch(const Readiness& r) noexcept
    {
        return r.innerVersion==274 && r.modernOwner && r.decodedVariant && r.independentUVs &&
            r.extendedVertexABI && r.blendResolved && r.uniformsReady &&
            (r.combine==Combine::TwoColorThreeAlpha || r.combine==Combine::ThreeColorThreeAlpha) &&
            (r.lighting==Lighting::Unlit || r.lighting==Lighting::Directional || r.lighting==Lighting::DirectionalPoint || r.lighting==Lighting::DirectionalTwoPoints) &&
            r.textures[0] && r.textures[1] && r.textures[2];
    }

    // Coordinates here are ALREADY decoded to texture-space units by a verified source adapter.
    // Raw 0x1DC scroll words / 0x2C scale bytes cannot be passed to this API as floats.
    struct LayerMotion { Vec2 start, velocity, scale; };
    inline bool EvaluateUV(Vec2 corner, const LayerMotion& motion, float particleAge, Vec2& out) noexcept
    {
        if (!std::isfinite(particleAge) || particleAge<0 || !std::isfinite(corner.x) || !std::isfinite(corner.y) ||
            !std::isfinite(motion.start.x) || !std::isfinite(motion.start.y) ||
            !std::isfinite(motion.velocity.x) || !std::isfinite(motion.velocity.y) ||
            !std::isfinite(motion.scale.x) || !std::isfinite(motion.scale.y)) return false;
        const Vec2 value{motion.start.x+motion.velocity.x*particleAge+corner.x*motion.scale.x,
                         motion.start.y+motion.velocity.y*particleAge+corner.y*motion.scale.y};
        if (!std::isfinite(value.x) || !std::isfinite(value.y)) return false;
        out=value; // Do not wrap here: sampler addressing belongs to the material.
        return true;
    }
    struct Parameters { float colorMultiplier=1, alphaMultiplier=1, alphaCutoff=0; };
    // Opt-in reference interpretation of MultiTexture / MultitexUseModx4. Caller must
    // first pass source ownership, version, blend and ABI guards. RGB-only placement
    // is an experimental contract: donor alpha/modulation ordering is not recovered.
    inline float SourceRGBModulationCandidate(bool enabled, uint32_t sourceFlags) noexcept
    {
        if (!enabled || !(sourceFlags & 0x10000000u)) return 1.0f;
        return (sourceFlags & 0x20000000u) ? 4.0f : 2.0f;
    }
    inline bool Valid(const Parameters& p) noexcept
    {
        return std::isfinite(p.colorMultiplier) && p.colorMultiplier>=0 &&
            std::isfinite(p.alphaMultiplier) && p.alphaMultiplier>=0 &&
            std::isfinite(p.alphaCutoff) && p.alphaCutoff>=0 && p.alphaCutoff<=1;
    }
    // CPU oracle for the HLSL kernels. Input color is already lit. No guessed source-flag mapping,
    // alpha premultiplication, refraction, source UV variant, or framebuffer blend is supplied.
    inline Color CombineLayers(Combine mode, Color color, const std::array<Color,3>& t,
                               const Parameters& p) noexcept
    {
        if (mode!=Combine::TwoColorThreeAlpha && mode!=Combine::ThreeColorThreeAlpha) return {};
        const Color last=mode==Combine::ThreeColorThreeAlpha ? t[2] : Color{1,1,1,t[2].a};
        return {color.r*t[0].r*t[1].r*last.r*p.colorMultiplier,
                color.g*t[0].g*t[1].g*last.g*p.colorMultiplier,
                color.b*t[0].b*t[1].b*last.b*p.colorMultiplier,
                color.a*t[0].a*t[1].a*t[2].a*p.alphaMultiplier};
    }
    inline Color ApplyNativeFog(Color value, Vec3 fog, float visibility) noexcept
    {
        return {(value.r-fog.x)*visibility+fog.x, (value.g-fog.y)*visibility+fog.y,
                (value.b-fog.z)*visibility+fog.z, value.a};
    }

    // Conservative future batching key. Same textures alone are insufficient. Identity tokens
    // must outlive the draw and include generation; uniformsIdentity names an immutable snapshot.
    struct BatchKey
    {
        uintptr_t modelGeneration=0, uniformsIdentity=0;
        std::array<uintptr_t,3> textures{};
        std::array<uint32_t,3> samplerIdentity{};
        uint32_t sourceFlags=0, resolvedBlend=0, vertexStride=0;
        Combine combine=Combine::Unsupported;
        Lighting lighting=Lighting::Unsupported;
    };
    inline bool CanMerge(const BatchKey& a, const BatchKey& b) noexcept
    {
        if (!a.modelGeneration || !a.uniformsIdentity || !a.textures[0] || !a.textures[1] || !a.textures[2] ||
            (a.combine!=Combine::TwoColorThreeAlpha && a.combine!=Combine::ThreeColorThreeAlpha) ||
            (a.lighting!=Lighting::Unlit && a.lighting!=Lighting::Directional && a.lighting!=Lighting::DirectionalPoint) ||
            a.vertexStride!=(a.lighting==Lighting::Unlit ? sizeof(UnlitVertex) : sizeof(LitVertex))) return false;
        return a.modelGeneration==b.modelGeneration && a.uniformsIdentity==b.uniformsIdentity &&
            a.textures==b.textures && a.samplerIdentity==b.samplerIdentity && a.sourceFlags==b.sourceFlags &&
            a.resolvedBlend==b.resolvedBlend && a.vertexStride==b.vertexStride &&
            a.combine==b.combine && a.lighting==b.lighting;
    }
}
