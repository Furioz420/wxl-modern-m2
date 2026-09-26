// Local snapshot facade over wxl-db2's ABI-safe, demand-resolved retail display graph.
// Copyright (C) 2026 WarcraftXL. GPLv3.

#include "ItemDisplayIndex.hpp"

#include "client/CharModel/RetailSkinProvider.hpp"
#include "ExtensionApi.hpp"

#include <algorithm>
#include <mutex>

namespace wxl::runtime::db2::itemdisplay
{
    namespace
    {
        std::mutex g_mutex;
        std::shared_ptr<const Index> g_current;
        uint64_t g_generation = 0;

        const char* Text(Index& index, const char* value)
        {
            return index.Intern(value ? value : "");
        }

        std::string ComponentPath(const MaterialEntry& entry)
        {
            if (!entry.texture || !*entry.texture) return {};
            std::string path = entry.folder ? entry.folder : "";
            path += entry.texture;
            if (path.size() < 4 || path.substr(path.size() - 4) != ".blp") path += ".blp";
            return path;
        }

        std::shared_ptr<const Index> Refresh(const WXL_RetailDb2Api& api, uint64_t generation)
        {
            void* lease = api.AcquireIndex ? api.AcquireIndex() : nullptr;
            if (!lease) return g_current;
            struct Release
            {
                const WXL_RetailDb2Api& api;
                void* lease;
                ~Release() { api.ReleaseIndex(lease); }
            } release{api, lease};

            auto next = g_current ? std::make_shared<Index>(*g_current) : std::make_shared<Index>();
            // String pointers in a copied Index must be re-interned into this snapshot before any
            // prior snapshot can be released. Refresh every resolved display on publication.
            next->strings.clear();
            next->models.clear();
            next->materials.clear();
            next->displayRecords.clear();
            next->resolvedDisplays.clear();
            auto helmetData = std::make_shared<HelmetData>();
            next->helmetData = helmetData;
            next->modelsReady = api.IndexModelsReady(lease) != 0;
            next->materialsReady = api.IndexMaterialsReady(lease) != 0;

            const uint32_t resolvedCount = api.IndexResolvedDisplayCount(lease);
            for (uint32_t i = 0; i < resolvedCount; ++i)
            {
                uint32_t displayId = 0;
                if (!api.IndexResolvedDisplayAt(lease, i, &displayId)) continue;
                next->resolvedDisplays.insert(displayId);

                WXL_RetailDisplayRecord rawRecord{};
                if (api.IndexDisplayRecord(lease, displayId, &rawRecord))
                {
                    DisplayRecord record;
                    record.inventoryType = rawRecord.inventoryType;
                    record.flags = rawRecord.flags;
                    record.itemVisual = rawRecord.itemVisual;
                    record.particleColor = rawRecord.particleColor;
                    for (size_t column = 0; column < 2; ++column)
                    {
                        record.nativeModelNames[column] = Text(*next, rawRecord.nativeModelNames[column]);
                        record.nativeModelTextures[column] = Text(*next, rawRecord.nativeModelTextures[column]);
                        record.helmetVis[column] = rawRecord.helmetVis[column];
                    }
                    std::copy(std::begin(rawRecord.geosets), std::end(rawRecord.geosets),
                              record.geosets.begin());
                    for (size_t layer = 0; layer < 8; ++layer)
                        record.componentTextures[layer] = Text(*next, rawRecord.componentTextures[layer]);
                    next->displayRecords.emplace(displayId, record);
                }

                const uint32_t modelCount = api.IndexModelCount(lease, displayId);
                auto& models = next->models[displayId];
                models.reserve(modelCount);
                for (uint32_t modelIndex = 0; modelIndex < modelCount; ++modelIndex)
                {
                    WXL_RetailModelEntry raw{};
                    if (!api.IndexModelAt(lease, displayId, modelIndex, &raw)) continue;
                    ModelEntry entry;
                    entry.modelSlot = raw.modelSlot;
                    entry.attachId = raw.attachId;
                    entry.modelIndex = raw.modelIndex;
                    entry.raceId = raw.raceId;
                    entry.genderId = raw.genderId;
                    entry.modelFlags = raw.modelFlags;
                    entry.textureFlags = raw.textureFlags;
                    entry.folder = Text(*next, raw.folder);
                    entry.model = Text(*next, raw.model);
                    entry.texture = Text(*next, raw.texture);
                    std::copy(std::begin(raw.geoFilter.ids), std::end(raw.geoFilter.ids),
                              std::begin(entry.geoFilter.ids));
                    entry.geoFilter.count = raw.geoFilter.count;
                    models.push_back(entry);
                }

                const uint32_t materialCount = api.IndexMaterialCount(lease, displayId);
                auto& materials = next->materials[displayId];
                materials.reserve(materialCount);
                for (uint32_t materialIndex = 0; materialIndex < materialCount; ++materialIndex)
                {
                    WXL_RetailMaterialEntry raw{};
                    if (!api.IndexMaterialAt(lease, displayId, materialIndex, &raw)) continue;
                    MaterialEntry entry;
                    entry.modelIndex = raw.modelIndex;
                    entry.modelColumn = raw.modelColumn;
                    entry.layer = raw.layer;
                    entry.textureType = raw.textureType;
                    entry.raceId = raw.raceId;
                    entry.genderId = raw.genderId;
                    entry.folder = Text(*next, raw.folder);
                    entry.model = Text(*next, raw.model);
                    entry.texture = Text(*next, raw.texture);
                    entry.skinSectionIds = Text(*next, raw.skinSectionIds);
                    entry.batchIndexes = Text(*next, raw.batchIndexes);
                    entry.targetSkinSectionIds = Text(*next, raw.targetSkinSectionIds);
                    entry.targetBatchIndexes = Text(*next, raw.targetBatchIndexes);
                    entry.targetMode = Text(*next, raw.targetMode);
                    materials.push_back(entry);
                    if (entry.layer < 8)
                    {
                        const std::string path = ComponentPath(entry);
                        if (!path.empty())
                            wxl::client::charmodel::RegisterRetailComponentTexturePath(path, entry.layer);
                    }
                }

                const auto record = next->displayRecords.find(displayId);
                if (record == next->displayRecords.end()) continue;
                for (uint32_t visibilityId : record->second.helmetVis)
                {
                    if (!visibilityId) continue;
                    const uint32_t ruleCount = api.IndexHelmetRuleCount(lease, visibilityId);
                    auto& rules = helmetData->geosetsByVis[visibilityId];
                    rules.clear();
                    rules.reserve(ruleCount);
                    for (uint32_t ruleIndex = 0; ruleIndex < ruleCount; ++ruleIndex)
                    {
                        WXL_RetailHelmetGeosetRule raw{};
                        if (api.IndexHelmetRuleAt(lease, visibilityId, ruleIndex, &raw))
                            rules.push_back(HelmetGeosetRule{
                                raw.raceId, raw.hideGroup, raw.raceBitSelection, raw.flags,
                            });
                    }
                    for (uint32_t raceId = 1; raceId <= 64; ++raceId)
                    {
                        float scale = 0.0f;
                        if (api.IndexHelmetAnimScale(lease, visibilityId, raceId, &scale))
                            helmetData->animScaleByVisRace[
                                (static_cast<uint64_t>(visibilityId) << 32) | raceId] = scale;
                    }
                }
            }
            g_generation = generation;
            return next;
        }
    }

    const char* Index::Intern(std::string_view value)
    {
        if (value.empty()) return "";
        return strings.emplace(value).first->c_str();
    }

    const std::vector<ModelEntry>* Index::FindModels(uint32_t displayId) const noexcept
    {
        const auto found = models.find(displayId);
        return found == models.end() ? nullptr : &found->second;
    }

    void Publish(std::shared_ptr<Index> index)
    {
        const std::lock_guard lock(g_mutex);
        g_current = std::move(index);
    }

    std::shared_ptr<const Index> Current()
    {
        const std::lock_guard lock(g_mutex);
        const WXL_RetailDb2Api* api = wxl_modern_m2::RetailDb2();
        if (!api || !api->Enabled || !api->Enabled()) return {};
        const uint64_t generation = api->IndexGeneration();
        if (!g_current || generation != g_generation)
            g_current = Refresh(*api, generation);
        return g_current;
    }

    uint64_t Generation() noexcept
    {
        const std::lock_guard lock(g_mutex);
        return g_generation;
    }

    void Request(uint32_t displayId)
    {
        RequestBatch(std::span<const uint32_t>(&displayId, 1));
    }

    void RequestBatch(std::span<const uint32_t> displayIds)
    {
        const WXL_RetailDb2Api* api = wxl_modern_m2::RetailDb2();
        if (api && api->RequestDisplays && !displayIds.empty())
            api->RequestDisplays(displayIds.data(), static_cast<uint32_t>(displayIds.size()));
    }

    std::vector<uint32_t> WaitTakeRequests() { return {}; }
    std::vector<uint32_t> TakeRequests() { return {}; }
    void FinishRequests(std::span<const uint32_t>) {}
}
