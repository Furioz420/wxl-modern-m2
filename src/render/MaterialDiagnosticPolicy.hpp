// Copyright (C) 2026 WarcraftXL. GPL-3.0-or-later.
#pragma once
#include "../compat/SourceMaterialCapture.hpp"
#include <array>
#include <string_view>

namespace wxl::modern::materialdiag
{
    inline char Fold(char c) noexcept
    {
        if (c == '/') return '\\';
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c;
    }
    inline bool MatchesPath(std::string_view path, std::string_view filter) noexcept
    {
        if (filter.empty()) return true;
        if (filter.size() > path.size()) return false;
        for (size_t n = 0; n <= path.size() - filter.size(); ++n)
        {
            size_t i = 0;
            while (i < filter.size() && Fold(path[n + i]) == Fold(filter[i])) ++i;
            if (i == filter.size()) return true;
        }
        return false;
    }

    struct ProbeKey
    {
        const void* owner = nullptr;
        const void* instance = nullptr;
        const void* skin = nullptr;
        const void* batches = nullptr;
        uint32_t batch = 0;
        bool operator==(const ProbeKey& b) const noexcept
        {
            return owner == b.owner && instance == b.instance && skin == b.skin &&
                batches == b.batches && batch == b.batch;
        }
    };
    // Render-thread-owned; fixed memory and bounded logging. Pointer reuse can suppress a
    // diagnostic, never select a material. Restart resets the sampling window.
    class ProbeBudget
    {
        struct Slot { ProbeKey key; uint32_t tick = 0, samples = 0; };
        std::array<Slot, 64> slots_{};
        uint32_t used_ = 0, total_ = 0;
        uint32_t limit_ = 32, perKey_ = 2, interval_ = 250;
    public:
        ProbeBudget() = default;
        ProbeBudget(uint32_t limit, uint32_t perKey, uint32_t interval)
            : limit_(limit > 64 ? 64 : limit), perKey_(perKey > 4 ? 4 : perKey), interval_(interval) {}
        bool Exhausted() const noexcept { return total_ >= limit_ || perKey_ == 0; }
        uint32_t Total() const noexcept { return total_; }
        bool Take(const ProbeKey& key, uint32_t tick) noexcept
        {
            if (Exhausted()) return false;
            for (uint32_t n = 0; n < used_; ++n)
                if (slots_[n].key == key)
                {
                    auto& slot = slots_[n];
                    if (slot.samples >= perKey_ || uint32_t(tick - slot.tick) < interval_) return false;
                    slot.tick = tick; ++slot.samples; ++total_;
                    return true;
                }
            if (used_ == slots_.size()) return false;
            slots_[used_++] = {key, tick, 1};
            ++total_;
            return true;
        }
    };

    struct LookupValue { bool available = false; uint16_t value = 0; };
    inline LookupValue Lookup(const std::vector<uint16_t>& values, uint16_t base, uint32_t stage) noexcept
    {
        // Stage is independently bounded; never wrap a 16-bit combo base plus a stage.
        if (stage >= 4) return {};
        const uint32_t index = uint32_t(base) + stage;
        return index < values.size() ? LookupValue{true, values[index]} : LookupValue{};
    }
    struct SourceStage
    {
        bool inBatch = false, textureValid = false, txidAvailable = false;
        LookupValue texture, coord, weight, transform;
        uint32_t type = 0, flags = 0, txid = 0;
    };
    inline SourceStage InspectStage(const assets::m2::material::ModelSource& source,
        const wxl::structure::m2::M2Batch& batch, uint32_t stage) noexcept
    {
        SourceStage out;
        if (stage >= 4 || stage >= batch.textureCount) return out;
        out.inBatch = true;
        out.texture = Lookup(source.textureCombos, batch.textureComboIndex, stage);
        out.coord = Lookup(source.coordCombos, batch.textureCoordComboIndex, stage);
        out.weight = Lookup(source.weightCombos, batch.textureWeightComboIndex, stage);
        out.transform = Lookup(source.transformCombos, batch.textureTransformComboIndex, stage);
        if (out.texture.available && out.texture.value < source.textures.size())
        {
            out.textureValid = true;
            out.type = source.textures[out.texture.value].type;
            out.flags = source.textures[out.texture.value].flags;
            if (out.texture.value < source.textureFileDataIds.size())
            {
                out.txidAvailable = true;
                out.txid = source.textureFileDataIds[out.texture.value];
            }
        }
        return out;
    }
}
