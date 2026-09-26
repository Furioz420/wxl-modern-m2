// Copyright (C) 2026 WarcraftXL. GPL-3.0-or-later.
#include "MaterialBlendExperiment.hpp"
#include "MaterialBlendPolicy.hpp"
#include "../compat/MaterialConfig.hpp"
#include "../ExtensionApi.hpp"
#include "../compat/ModernM2.hpp"
#include "game/M2.hpp"
#include "offsets/game/M2.hpp"

namespace wxl::modern::materialblend
{
    namespace off = wxl::offsets::game::m2;
    namespace mat = assets::m2::material;
    namespace
    {
        bool enabled = false;
        unsigned reports = 0;
        struct Context
        {
            void* shared = nullptr;
            const void* owner = nullptr;
            const void* skin = nullptr;
            const void* batches = nullptr;
            uint32_t count = 0, index = 0;
            uint16_t blend = 0;
            wxl::structure::m2::M2Batch batch{};
        };
        bool ReadContext(void* raw, void* instance, Context& out) noexcept
        {
            if (!raw || !instance) return false;
            __try
            {
                const auto* dc = static_cast<const off::DrawContext*>(raw);
                if (dc->instance != instance || !dc->element || !dc->material) return false;
                out.shared = reinterpret_cast<void*>(static_cast<off::M2Instance*>(instance)->model);
                if (!out.shared) return false;
                wxl::game::m2::M2Model model(out.shared);
                const auto* skin = model.GetSkin();
                out.owner = model.GetHeader();
                if (!out.owner || !skin || !skin->batches || !skin->batchCount || skin->batchCount > 0x10000)
                    return false;
                out.index = *reinterpret_cast<const uint32_t*>(
                    static_cast<const uint8_t*>(dc->element) + off::kOffElementBatchIndex);
                if (out.index >= skin->batchCount) return false;
                out.skin = skin;
                out.batches = skin->batches;
                out.count = skin->batchCount;
                out.batch = skin->batches[out.index];
                out.blend = static_cast<const off::Material*>(dc->material)->blend;
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }
        bool Select(void* raw, void* instance, Context& context) noexcept
        {
            try
            {
                if (!ReadContext(raw, instance, context) || context.blend != 4 ||
                    !assets::m2::IsNativeLoaded(context.shared)) return false;
                const auto source = mat::SourceMaterials().Find(context.owner, context.skin,
                    context.batches, context.count);
                return source && Eligible(*source, context.index, context.batch, context.blend);
            }
            catch (...) { return false; }
        }
        template<class T> struct ComRead
        {
            T* value = nullptr;
            ~ComRead() { if (value) value->Release(); }
        };
        bool HasProgrammableTextures(IDirect3DDevice9* device) noexcept
        {
            ComRead<IDirect3DVertexShader9> vs;
            ComRead<IDirect3DPixelShader9> ps;
            ComRead<IDirect3DBaseTexture9> t0, t1;
            return SUCCEEDED(device->GetVertexShader(&vs.value)) && vs.value &&
                SUCCEEDED(device->GetPixelShader(&ps.value)) && ps.value &&
                SUCCEEDED(device->GetTexture(0, &t0.value)) && t0.value &&
                SUCCEEDED(device->GetTexture(1, &t1.value)) && t1.value;
        }
    }
    void Initialize() noexcept
    {
        try
        {
            enabled = materialconfig::Feature("WXL_M2_BLEND7_EXPERIMENT");
            reports = 0;
            if (enabled) WLOG_WARN("m2-blend7-experiment: enabled; framebuffer-only A/B, not full material support; native shaders retained");
        }
        catch (...) { enabled = false; }
    }
    long Draw(void* rawDevice, void* rawContext, void* instance, int type, int baseVertex,
              unsigned minVertex, unsigned vertices, unsigned start, unsigned primitives,
              DrawFn original)
    {
        Context context{};
        if (!enabled || !rawDevice || !Select(rawContext, instance, context))
            return original(rawDevice, type, baseVertex, minVertex, vertices, start, primitives);
        auto* device = static_cast<IDirect3DDevice9*>(rawDevice);
        if (!HasProgrammableTextures(device))
            return original(rawDevice, type, baseVertex, minVertex, vertices, start, primitives);
        ScopedBlend<IDirect3DDevice9> states(device);
        const bool applied = states.Apply();
        if (reports < 24)
        {
            ++reports;
            WLOG_INFO("m2-blend7-experiment: sample=%u owner=%p batch=%u applied=%u sourceBlend=7 liveBlend=4"
                " shader=0x4014 src=ONE dst=INVSRCALPHA; pre-DIP probe records original state",
                reports, context.owner, context.index, unsigned(applied));
        }
        const long result = original(rawDevice, type, baseVertex, minVertex, vertices, start, primitives);
        if (!states.Restore())
        {
            enabled = false;
            WLOG_WARN("m2-blend7-experiment: state restore failed; disabled for this process; restart client before more comparisons");
        }
        return result;
    }
}
