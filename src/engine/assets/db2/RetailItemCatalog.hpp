// Immutable retail item metadata consumed by native DBC-first item accessors.
// Copyright (C) 2026 WarcraftXL. GPLv3.

#pragma once

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <unordered_set>

namespace wxl::runtime::db2::retailitems
{
    struct Item
    {
        uint32_t classId = 0;
        uint32_t subclassId = 0;
        uint32_t soundOverride = 0;
        uint32_t itemGroupSoundsId = 0;
        uint32_t material = 0;
        uint32_t inventoryType = 0;
        uint32_t sheatheType = 0;
        uint32_t displayId = 0;
        uint32_t appearanceId = 0;
        uint32_t iconFileDataId = 0;
    };

    struct Catalog
    {
        std::unordered_map<uint32_t, Item> items;
        std::unordered_map<uint32_t, uint32_t> iconByDisplay;
        std::unordered_map<uint32_t, uint32_t> soundByDisplay;
        std::unordered_set<uint32_t> displayIds;

        struct Variant
        {
            uint32_t displayId = 0;
            uint32_t appearanceId = 0;
            uint32_t iconFileDataId = 0;
        };
        std::unordered_map<uint64_t, Variant> variants;
    };

    void Publish(std::shared_ptr<Catalog> catalog);
    std::shared_ptr<const Catalog> Current();
}
