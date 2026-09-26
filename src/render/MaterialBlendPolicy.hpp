// Copyright (C) 2026 WarcraftXL. GPL-3.0-or-later.
#pragma once
#include "../compat/SourceMaterialCapture.hpp"
#include <d3d9.h>
#include <array>

namespace wxl::modern::materialblend
{
    // Experimental capability slice, not a model-name rule or a claim of complete Legion support.
    // Retain the native shader ABI. Only the unchanged two-texture packed family seen in the
    // September 5 capture is eligible; synthesized passes and bone-split outputs are excluded.
    inline bool Eligible(const assets::m2::material::SkinSource& source, uint32_t index,
                         const wxl::structure::m2::M2Batch& live, uint16_t liveBlend) noexcept
    {
        if (!source.valid || !source.model || index >= source.outputs.size() || liveBlend != 4)
            return false;
        const auto& model = *source.model;
        if (model.containerMagic != wxl::structure::m2::kMagicMD21 || model.innerVersion != 274)
            return false;
        const auto& origin = source.outputs[index];
        if (origin.parked || origin.piece || origin.split || origin.sourceBatch >= source.batches.size())
            return false;
        const auto& raw = source.batches[origin.sourceBatch];
        if (raw.materialIndex >= model.materials.size() || model.materials[raw.materialIndex].blend != 7)
            return false;
        return raw.shaderId == 0x4014 && live.shaderId == raw.shaderId &&
            raw.textureCount == 2 && live.textureCount == raw.textureCount &&
            live.materialIndex == raw.materialIndex && live.colorIndex == raw.colorIndex &&
            live.textureComboIndex == raw.textureComboIndex &&
            live.textureCoordComboIndex == raw.textureCoordComboIndex &&
            live.textureWeightComboIndex == raw.textureWeightComboIndex &&
            live.textureTransformComboIndex == raw.textureTransformComboIndex;
    }

    struct StateValue { D3DRENDERSTATETYPE state; DWORD value; };
    // M2 blend 7 -> Gx BlendAdd: C = Cs + Cd*(1-As), A = As + Ad*(1-As).
    // Color and alpha use identical factors, so disable separate alpha rather than requiring
    // that optional capability. Alpha-test, fog, lighting and shader output remain unchanged:
    // this deliberately isolates framebuffer blending, not full material correctness.
    inline constexpr std::array<StateValue, 5> kBlendAddStates{{
        {D3DRS_ALPHABLENDENABLE, TRUE}, {D3DRS_SRCBLEND, D3DBLEND_ONE},
        {D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA}, {D3DRS_BLENDOP, D3DBLENDOP_ADD},
        {D3DRS_SEPARATEALPHABLENDENABLE, FALSE}
    }};

    // Used at the final DIP boundary, after Gx's deferred state flush. Restore the exact
    // previous device state before any subscriber runs so its cache remains coherent.
    template<class Device> class ScopedBlend
    {
        Device* device_;
        std::array<DWORD, kBlendAddStates.size()> saved_{};
        bool needsRestore_ = false;
        bool restoreOk_ = true;
    public:
        explicit ScopedBlend(Device* device) noexcept : device_(device) {}
        ScopedBlend(const ScopedBlend&) = delete;
        ScopedBlend& operator=(const ScopedBlend&) = delete;
        ~ScopedBlend() { Restore(); }
        bool Apply() noexcept
        {
            if (!device_ || needsRestore_ || !restoreOk_) return false;
            // Do not mutate anything unless every original state can be recovered.
            for (size_t i = 0; i < saved_.size(); ++i)
                if (FAILED(device_->GetRenderState(kBlendAddStates[i].state, &saved_[i]))) return false;
            needsRestore_ = true;
            for (const auto& entry : kBlendAddStates)
                if (FAILED(device_->SetRenderState(entry.state, entry.value))) { Restore(); return false; }
            for (const auto& entry : kBlendAddStates)
            {
                DWORD actual = 0;
                if (FAILED(device_->GetRenderState(entry.state, &actual)) || actual != entry.value)
                { Restore(); return false; }
            }
            return true;
        }
        bool Restore() noexcept
        {
            if (!needsRestore_) return restoreOk_;
            needsRestore_ = false;
            // Attempt every restoration even after one driver error.
            for (size_t i = saved_.size(); i-- > 0;)
                if (FAILED(device_->SetRenderState(kBlendAddStates[i].state, saved_[i]))) restoreOk_ = false;
            return restoreOk_;
        }
    };
}
