// Shared DB2-derived item display attachments and material targeting.
// Copyright (C) 2026 WarcraftXL. GPLv3.

#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace wxl::runtime::db2::itemdisplay
{
    struct GeosetFilter
    {
        uint16_t ids[16]{};
        uint32_t count = 0;
    };

    struct ModelEntry
    {
        uint32_t modelSlot = static_cast<uint32_t>(-1);
        uint32_t attachId = static_cast<uint32_t>(-1);
        uint32_t modelIndex = static_cast<uint32_t>(-1);
        uint32_t raceId = 0;
        uint32_t genderId = static_cast<uint32_t>(-1);
        uint32_t modelFlags = 0x2u | 0x4u;
        uint32_t textureFlags = 0;
        const char* folder = "";
        const char* model = "";
        const char* texture = "";
        GeosetFilter geoFilter{};
    };

    struct MaterialEntry
    {
        uint32_t modelIndex = static_cast<uint32_t>(-1);
        uint32_t modelColumn = static_cast<uint32_t>(-1);
        uint32_t layer = static_cast<uint32_t>(-1);
        uint32_t textureType = static_cast<uint32_t>(-1);
        uint32_t raceId = 0;
        uint32_t genderId = static_cast<uint32_t>(-1);
        const char* folder = "";
        const char* model = "";
        const char* texture = "";
        const char* skinSectionIds = "";
        const char* batchIndexes = "";
        const char* targetSkinSectionIds = "";
        const char* targetBatchIndexes = "";
        const char* targetMode = "";
    };

    struct DisplayRecord
    {
        uint32_t inventoryType = 0;
        uint32_t flags = 0;
        uint32_t itemVisual = 0;
        uint32_t particleColor = 0;
        // WotLK's native visible-item owner still owns weapon/offhand/ranged models. Armor model
        // strings remain empty because modern armor is attached by the native v1.1 controller.
        std::array<const char*, 2> nativeModelNames{};
        std::array<const char*, 2> nativeModelTextures{};
        std::array<uint32_t, 6> geosets{};
        std::array<uint32_t, 2> helmetVis{};
        std::array<const char*, 8> componentTextures{};
    };

    struct HelmetGeosetRule
    {
        uint32_t raceId = 0;
        uint32_t hideGroup = 0;
        uint32_t raceBitSelection = 0;
        uint32_t flags = 0;
    };

    struct HelmetData
    {
        std::unordered_map<uint32_t, std::vector<HelmetGeosetRule>>
            geosetsByVis;
        std::unordered_map<uint64_t, float> animScaleByVisRace;
    };

    struct Index
    {
        using ModelMap = std::unordered_map<uint32_t, std::vector<ModelEntry>>;

        ModelMap models;
        std::unordered_map<uint32_t, std::vector<MaterialEntry>> materials;
        std::unordered_map<uint32_t, DisplayRecord> displayRecords;
        std::shared_ptr<const HelmetData> helmetData;
        std::unordered_set<uint32_t> resolvedDisplays;
        std::unordered_set<std::string> strings;
        bool modelsReady = false;
        bool materialsReady = false;

        const char* Intern(std::string_view value);
        const std::vector<ModelEntry>* FindModels(uint32_t displayId) const noexcept;
    };

    /** Atomically publishes an immutable model-only or complete snapshot. */
    void Publish(std::shared_ptr<Index> index);

    /** Returns the current snapshot, or null while the DB2 background index is still building. */
    std::shared_ptr<const Index> Current();

    /** Queues an unresolved display for the background item graph worker. */
    void Request(uint32_t displayId);

    /**
     * Queues a complete display set under one lock and wakes the graph worker once.
     * This is used by one-shot model builders (notably Glue character selection),
     * where publishing the whole equipment graph before native composition is both
     * faster and more correct than resolving one slot at a time.
     */
    void RequestBatch(std::span<const uint32_t> displayIds);

    /** Blocks until at least one unresolved display is requested, then drains the queue. */
    std::vector<uint32_t> WaitTakeRequests();

    /** Drains currently queued unresolved displays without blocking. */
    std::vector<uint32_t> TakeRequests();

    /** Releases display IDs owned by the worker after publication or failure. */
    void FinishRequests(std::span<const uint32_t> displayIds);
}
