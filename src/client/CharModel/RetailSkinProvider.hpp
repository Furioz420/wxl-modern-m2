// Per-slot virtual SKIN resources for retail collection models.
// Copyright (C) 2026 WarcraftXL. GPLv3.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace wxl::client::charmodel
{
    // Diagnostic snapshots count retained vector capacity, excluding map/string overhead.
    struct RetailSkinMemoryStats
    {
        uint64_t virtualBytes = 0;
        uint64_t componentBytes = 0;
        size_t virtualFiles = 0;
        size_t componentFiles = 0;
        size_t preparedPaths = 0;
        size_t componentAliases = 0;
    };
    RetailSkinMemoryStats GetRetailSkinMemoryStats();

    /**
     * Registers the name-based 3.3.5 skin request for a modern model whose
     * base SKIN is stored under Retail's FileDataID-suffixed companion name.
     * Existing name-addressable companions are left untouched.
     */
    bool RegisterRetailSkinCompanion(std::string_view modelPath,
                                     uint32_t skinFileDataId);

    /**
     * Registers the collision-safe retail path for a character component
     * texture. The native WotLK compositor requests derived legacy aliases
     * (including its _M/_F/_U selector), which the provider maps back to this
     * exact mounted BLP.
     */
    void RegisterRetailComponentTexturePath(std::string_view path,
                                            uint32_t componentSection);

    /**
     * Builds a stable virtual M2 path whose 00.skin contains only the sections
     * owned by inventoryType. Different inventory types that select the same
     * sections from the same source receive the same path, allowing the
     * equipment owner to deduplicate overlapping robe/legs geometry. Returns
     * false only when the source cannot be read or validated.
     */
    bool PrepareRetailSkinPath(const char* realPath, uint32_t inventoryType,
                               char* outPath, size_t outSize) noexcept;
}
