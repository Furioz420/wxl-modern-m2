// Restricted FrameScript bridge state consumed by native client owners.
// Copyright (C) 2026 WarcraftXL. GPLv3.

#pragma once

#include <cstdint>

namespace wxl::client::wxlwow
{
    /**
     * Changes whenever the server-owned retail item-instance snapshot changes.
     * Native presentation owners use this to refresh same-item appearance
     * changes that do not alter WoW's visible item entry.
     */
    uint32_t VariantGeneration() noexcept;

    /**
     * Reads one equipped item instance from the latest server snapshot.
     *
     * @param equipmentSlot  One-based FrameScript equipment slot (1..19).
     */
    bool EquippedVariant(uint32_t equipmentSlot, uint32_t& itemId,
                         uint32_t& modifierId) noexcept;

    /**
     * Returns the server-owned modifier for a visible retail item. Equipped
     * instances win; otherwise a unique matching bag modifier is returned.
     */
    uint32_t VisibleModifierForItem(uint32_t itemId) noexcept;

    /**
     * Returns the native/source item whose local presentation and equipment
     * classification belongs to a server-generated item variant.
     */
    uint32_t PresentationSourceForItem(uint32_t itemId) noexcept;
}
