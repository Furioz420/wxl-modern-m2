// Public native retail-item presentation helpers.
// Copyright (C) 2026 WarcraftXL. GPLv3.

#pragma once

#include <cstdint>
#include <string>

namespace wxl::client::retailitem
{
    /**
     * Returns true when Item.dbc and ItemDisplayInfo.dbc fully own an item.
     *
     * Merely sharing an ID with retail Item.db2 must never move a native
     * WotLK/custom DBC item onto the retail appearance path. A custom DBC item
     * whose display is deliberately absent from ItemDisplayInfo.dbc remains a
     * supported hybrid and returns false so DB2 can supply that display.
     */
    bool IsNativePresentation(uint32_t itemId) noexcept;

    /**
     * Remembers the retail appearance modifier associated with one live
     * PLAYER_VISIBLE_ITEM entry. The native world owner must see the real item
     * ID in that field; retail accessors recover the modifier through this
     * side channel.
     */
    void RememberVisibleModifier(const uint32_t* visibleItem,
                                 uint32_t itemId, uint32_t modifierId);

    /**
     * Queues the display graph for a retail item appearance without blocking.
     * Native WotLK items deliberately return zero and remain DBC-owned.
     *
     * @return The queued retail display ID, or zero when the item/appearance
     *         is unknown or belongs to the native DBC.
     */
    uint32_t RequestDisplayForItem(uint32_t itemId,
                                   uint32_t modifierId = 0) noexcept;

    /** Resolves an explicit character-creation outfit display absent from the native DBC.
     * Creation outfits may reference retired appearances with no current retail item.
     * Only the character-creation equipment owner should call this entry point.
     */
    bool RequestCharacterCreationDisplay(uint32_t displayId);

    /**
     * Queues an item's icon FileDataID for background resolution. This is
     * intentionally independent from the full display graph used by equipped
     * models.
     */
    void RequestIconForItem(uint32_t itemId,
                            uint32_t modifierId = 0) noexcept;

    /** Changes after a background icon batch becomes visible to FrameScript. */
    uint32_t IconGeneration() noexcept;

    /** Returns Interface\Icons\<stem> for an exact item appearance modifier. */
    std::string VariantIconPath(uint32_t itemId, uint32_t modifierId);

    /**
     * Returns the retail ItemDisplayInfo row most recently synthesized for the
     * current thread. Native weapon construction follows that lookup immediately.
     */
    uint32_t DisplayLookupContext() noexcept;

    /** Clears a display lookup after its native weapon model has claimed it. */
    void ClearDisplayLookupContext() noexcept;
}
