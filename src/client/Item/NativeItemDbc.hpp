// Supplemental WotLK-shaped item catalog helpers.
// Copyright (C) 2026 WarcraftXL. GPLv3.

#pragma once

#include <cstddef>
#include <cstdint>

namespace wxl::client::nativeitemdbc
{
    /**
     * Looks up an Item.dbc row in the immutable supplemental index without
     * replacing or mutating WoW's client-owned native DBC storage.
     */
    bool SupplementalItemDisplay(
        uint32_t itemId, uint32_t& displayId) noexcept;

    /** Reads one DWORD from a supplemental eight-field Item.dbc row. */
    bool SupplementalItemField(
        uint32_t itemId, size_t offset, uint32_t& value) noexcept;

    // Reads the original DBC male/female HelmetGeosetVis fields, including zero.
    bool SupplementalHelmetVisibility(uint32_t displayId, uint32_t sex, uint32_t& visibility) noexcept;

    /** True only for a verified display row with both model names empty. */
    bool SupplementalDisplayHasNoModel(uint32_t displayId) noexcept;

    /** Returns whether the complete host ItemDisplayInfo.dbc contains a row. */
    bool SupplementalDisplayExists(uint32_t displayId) noexcept;

    /**
     * Returns the inventory-icon stem declared by the authoritative loose
     * ItemDisplayInfo.dbc. The returned storage remains valid for the life of
     * the client.
     *
     * Native startup rows remain the first lookup tier. Consumers use this
     * index only when the compact client table does not contain displayId,
     * before considering retail DB2.
     */
    const char* SupplementalDisplayIcon(uint32_t displayId) noexcept;
}
