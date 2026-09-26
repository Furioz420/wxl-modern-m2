// Restricted WoW FrameScript bridge for server-authoritative retail item-instance variants.
// Copyright (C) 2026 WarcraftXL. GPLv3.

#include "ExtensionApi.hpp"
#include "client/FrameScript/WxlWow.hpp"
#include "client/Item/RetailItem.hpp"
#include "engine/assets/db2/RetailItemCatalog.hpp"
#include "game/Binding.hpp"
#include "offsets/engine/Lua.hpp"
#include "offsets/engine/Sound.hpp"

#include <windows.h>

#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <limits>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
    namespace luaoff = wxl::offsets::engine::lua;
    namespace soundoff = wxl::offsets::engine::sound;

    namespace opcodes
    {
        constexpr uint16_t CmsgRetailItemVariants = 0x0523;
        constexpr uint16_t SmsgRetailItemVariants = 0x0524;
    }

    namespace network
    {
        using LegacyHandler = void(*)(std::span<const uint8_t>, void*);

        struct HandlerBridge
        {
            LegacyHandler handler = nullptr;
        };

        std::deque<HandlerBridge> g_handlerBridges;

        void __cdecl Dispatch(const uint8_t* payload, uint32_t payloadSize,
                              void* user)
        {
            const auto* bridge = static_cast<const HandlerBridge*>(user);
            if (!bridge || !bridge->handler) return;
            bridge->handler(
                std::span<const uint8_t>(payload, payloadSize), nullptr);
        }

        bool RegisterClientOpcode(uint16_t opcode, const char* name)
        {
            const WXL_NetworkApi* api = wxl_modern_m2::Network();
            return api && api->RegisterClientOpcode(opcode, name) != 0;
        }

        bool RegisterServerOpcode(uint16_t opcode, const char* name,
                                  LegacyHandler handler)
        {
            const WXL_NetworkApi* api = wxl_modern_m2::Network();
            if (!api || !handler) return false;
            g_handlerBridges.push_back(HandlerBridge{handler});
            HandlerBridge* bridge = &g_handlerBridges.back();
            if (api->RegisterServerOpcode(
                    opcode, name, &Dispatch, bridge) != 0)
                return true;
            g_handlerBridges.pop_back();
            return false;
        }

        bool Send(uint16_t opcode)
        {
            const WXL_NetworkApi* api = wxl_modern_m2::Network();
            return api && api->Send(opcode, nullptr, 0) != 0;
        }

        template <size_t Size>
        bool Send(uint16_t opcode, const std::array<uint8_t, Size>& payload)
        {
            const WXL_NetworkApi* api = wxl_modern_m2::Network();
            return api && api->Send(
                opcode, payload.data(), static_cast<uint32_t>(payload.size())) != 0;
        }
    }

    namespace framescript
    {
        bool RegisterFunction(const char* name, WXL_LuaCFunction function)
        {
            const WXL_FrameScriptApi* api = wxl_modern_m2::FrameScript();
            return api && api->RegisterFunction(name, function) != 0;
        }

        bool RegisterScript(const char* name, const char* source)
        {
            const WXL_FrameScriptApi* api = wxl_modern_m2::FrameScript();
            return api && api->RegisterScript(name, source) != 0;
        }
    }

    constexpr uint32_t kMaxVariantCount = 512;
    constexpr uint32_t kMaxVariantLabelBytes = 128;
    constexpr uint32_t kMaxVariantPayloadBytes = 64u * 1024u;
    constexpr uint32_t kRequestThrottleMs = 100;
    constexpr uint16_t kVariantSnapshotMarker = 0xFFFF;
    constexpr uint8_t kVariantSnapshotVersion = 3;
    constexpr uint8_t kMaxSpellEffectOverrides = 15;
    constexpr uint32_t kMythicRewardItemStart = 10000000;

    struct SpellEffectOverride
    {
        uint32_t spellId = 0;
        uint8_t effectIndex = 0;
        int32_t sourceValue = 0;
        int32_t targetValue = 0;

        bool operator==(const SpellEffectOverride&) const = default;
    };

    struct Variant
    {
        uint32_t itemId = 0;
        uint32_t modifierId = 0;
        std::string label;
        uint32_t sourceItemId = 0;
        uint16_t itemLevel = 0;
        std::vector<SpellEffectOverride> spellEffectOverrides;
    };

    struct EquippedOverride
    {
        Variant variant;
        uint32_t createdAtMs = 0;
        bool active = false;
    };

    struct Reader
    {
        const uint8_t* data = nullptr;
        size_t size = 0;
        size_t cursor = 0;

        template <typename T>
        bool Read(T& value)
        {
            if (!data || cursor > size || sizeof(T) > size - cursor) return false;
            std::memcpy(&value, data + cursor, sizeof(T));
            cursor += sizeof(T);
            return true;
        }

        bool ReadString(std::string& value)
        {
            uint32_t length = 0;
            if (!Read(length) || length > kMaxVariantLabelBytes ||
                cursor > size || length > size - cursor)
                return false;
            value.assign(reinterpret_cast<const char*>(data + cursor), length);
            cursor += length;
            return true;
        }
    };

    std::mutex g_variantMutex;
    std::unordered_map<uint16_t, Variant> g_variants;
    std::array<EquippedOverride, 20> g_equippedOverrides{};
    std::atomic_uint32_t g_variantGeneration{0};
    uint32_t g_lastRequestMs = 0;

    uint16_t VariantKey(int8_t bag, uint8_t slot)
    {
        return static_cast<uint16_t>(
            (static_cast<uint16_t>(static_cast<uint8_t>(bag)) << 8) | slot);
    }

    uint32_t EquipmentSlotForInventoryType(uint32_t inventoryType) noexcept
    {
        switch (inventoryType)
        {
            case 1: return 1;   // Head
            case 3: return 3;   // Shoulder
            case 4: return 4;   // Shirt
            case 5: return 5;   // Chest
            case 6: return 6;   // Waist
            case 7: return 7;   // Legs
            case 8: return 8;   // Feet
            case 9: return 9;   // Wrist
            case 10: return 10; // Hands
            case 16: return 15; // Back
            case 19: return 19; // Tabard
            case 20: return 5;  // Robe
            default: return 0;
        }
    }

    bool ParseVariantSnapshot(const uint8_t* data, size_t size)
    {
        if (!data || size > kMaxVariantPayloadBytes) return false;

        Reader reader{data, size};
        uint8_t version = 1;
        uint16_t count = 0;
        uint16_t marker = 0;
        if (!reader.Read(marker)) return false;
        if (marker == kVariantSnapshotMarker)
        {
            if (!reader.Read(version) || version < 2 ||
                version > kVariantSnapshotVersion ||
                !reader.Read(count))
                return false;
        }
        else
            count = marker;
        if (count > kMaxVariantCount) return false;

        std::unordered_map<uint16_t, Variant> replacement;
        replacement.reserve(count);
        for (uint16_t index = 0; index < count; ++index)
        {
            int8_t bag = 0;
            uint8_t slot = 0;
            Variant variant;
            if (!reader.Read(bag) || !reader.Read(slot) ||
                !reader.Read(variant.itemId) || !reader.Read(variant.modifierId) ||
                !reader.ReadString(variant.label))
                return false;
            if (version >= 2 &&
                (!reader.Read(variant.sourceItemId) ||
                 !reader.Read(variant.itemLevel)))
                return false;
            if (version >= 3)
            {
                uint8_t effectCount = 0;
                if (!reader.Read(effectCount) ||
                    effectCount > kMaxSpellEffectOverrides)
                    return false;
                variant.spellEffectOverrides.reserve(effectCount);
                for (uint8_t effectIndex = 0; effectIndex < effectCount;
                    ++effectIndex)
                {
                    SpellEffectOverride effect;
                    if (!reader.Read(effect.spellId) ||
                        !reader.Read(effect.effectIndex) ||
                        !reader.Read(effect.sourceValue) ||
                        !reader.Read(effect.targetValue))
                        return false;
                    variant.spellEffectOverrides.push_back(effect);
                }
            }
            // Generated Mythic rewards deliberately reuse a source dungeon
            // item's presentation. Some client patches contain placeholder
            // Item.dbc rows for the generated high-ID range; treating those
            // placeholders as authoritative discards the server-owned label,
            // item level and source icon together.
            if (variant.itemId < kMythicRewardItemStart &&
                wxl::client::retailitem::IsNativePresentation(variant.itemId))
                continue;
            replacement[VariantKey(bag, slot)] = std::move(variant);
        }
        if (reader.cursor != reader.size) return false;

        // Only equipped slots need model/material graphs. Bag slots need icons
        // and tooltip metadata, both of which are already present in the retail
        // catalog; resolving their display graphs here makes opening a large bag
        // unnecessarily compete with visible character equipment.
        for (const auto& [key, variant] : replacement)
        {
            wxl::client::retailitem::RequestIconForItem(
                variant.sourceItemId ? variant.sourceItemId : variant.itemId,
                variant.modifierId);
            const int8_t bag = static_cast<int8_t>(key >> 8);
            if (bag >= 0) continue;
            wxl::client::retailitem::RequestDisplayForItem(
                variant.itemId, variant.modifierId);
        }

        bool changed = false;
        {
            const std::lock_guard lock(g_variantMutex);
            const uint32_t now = GetTickCount();
            for (uint32_t slot = 1;
                 slot < g_equippedOverrides.size(); ++slot)
            {
                EquippedOverride& pending = g_equippedOverrides[slot];
                if (!pending.active) continue;
                const auto authoritative =
                    replacement.find(VariantKey(
                        -1, static_cast<uint8_t>(slot)));
                if (authoritative == replacement.end()) continue;
                if (authoritative->second.itemId ==
                        pending.variant.itemId &&
                    authoritative->second.modifierId ==
                        pending.variant.modifierId)
                {
                    pending = {};
                    continue;
                }
                // An inventory event can request a snapshot while the equip
                // transaction is still being committed. Do not let that
                // preceding snapshot overwrite the local action.
                if (now - pending.createdAtMs >= 2000u)
                    pending = {};
            }

            changed = replacement.size() != g_variants.size();
            if (!changed)
            {
                for (const auto& [key, value] : replacement)
                {
                    const auto current = g_variants.find(key);
                    if (current == g_variants.end() ||
                        current->second.itemId != value.itemId ||
                        current->second.modifierId != value.modifierId ||
                        current->second.label != value.label ||
                        current->second.sourceItemId != value.sourceItemId ||
                        current->second.itemLevel != value.itemLevel ||
                        current->second.spellEffectOverrides !=
                            value.spellEffectOverrides)
                    {
                        changed = true;
                        break;
                    }
                }
            }
            g_variants = std::move(replacement);
        }
        if (changed)
        {
            g_variantGeneration.fetch_add(1, std::memory_order_release);
            WLOG_INFO("wxlwow: retail item-variant snapshot slots=%u", count);
            {
                const std::lock_guard lock(g_variantMutex);
                for (const auto& [key, value] : g_variants)
                {
                    if (value.itemId < kMythicRewardItemStart) continue;
                    WLOG_INFO(
                        "wxlwow: Mythic reward bag=%d slot=%u item=%u source=%u level=%u label=%s",
                        static_cast<int>(static_cast<int8_t>(key >> 8)),
                        static_cast<unsigned>(key & 0xFFu), value.itemId,
                        value.sourceItemId,
                        static_cast<unsigned>(value.itemLevel),
                        value.label.c_str());
                }
            }
            const auto catalog = wxl::runtime::db2::retailitems::Current();
            if (catalog)
            {
                const std::lock_guard lock(g_variantMutex);
                for (const auto& [key, value] : g_variants)
                {
                    if (static_cast<int8_t>(key >> 8) != -1 || !value.modifierId)
                        continue;
                    const uint64_t variantKey =
                        (static_cast<uint64_t>(value.itemId) << 32) |
                        value.modifierId;
                    const auto variant = catalog->variants.find(variantKey);
                    const auto fallback = catalog->items.find(value.itemId);
                    WLOG_INFO(
                        "wxlwow: equipped slot=%u item=%u modifier=%u label=%s display=%u default=%u",
                        static_cast<unsigned>(key & 0xFFu), value.itemId,
                        value.modifierId, value.label.c_str(),
                        variant == catalog->variants.end()
                            ? 0u : variant->second.displayId,
                        fallback == catalog->items.end()
                            ? 0u : fallback->second.displayId);
                }
            }
        }
        return true;
    }

    void OnVariantSnapshot(
        std::span<const uint8_t> payload, void*)
    {
        if (!ParseVariantSnapshot(payload.data(), payload.size()))
            WLOG_WARN(
                "wxlwow: rejected malformed retail item-variant snapshot bytes=%zu",
                payload.size());
    }

    bool SendVariantRequest()
    {
        const uint32_t now = GetTickCount();
        if (g_lastRequestMs && now - g_lastRequestMs < kRequestThrottleMs)
            return true;
        // Keep the empty v2 request first so a newer client continues to work
        // while the Core is being rolled out. A v3-aware Core follows it with
        // the extended effect-magnitude snapshot below.
        if (!network::Send(opcodes::CmsgRetailItemVariants)) return false;
        const std::array<uint8_t, 1> payload{kVariantSnapshotVersion};
        network::Send(opcodes::CmsgRetailItemVariants, payload);
        g_lastRequestMs = now;
        return true;
    }

    int __cdecl LuaRequestItemVariants(void* state)
    {
        wxl::game::Native<luaoff::LuaPushBooleanFn>(luaoff::kLuaPushBoolean)(
            state, SendVariantRequest() ? 1 : 0);
        return 1;
    }

    int __cdecl LuaItemVariant(void* state)
    {
        const auto toNumber =
            wxl::game::Native<luaoff::LuaToNumberFn>(luaoff::kLuaToNumber);
        const double bagNumber = toNumber(state, 1);
        const double slotNumber = toNumber(state, 2);
        if (!std::isfinite(bagNumber) || !std::isfinite(slotNumber) ||
            bagNumber < -128.0 || bagNumber > 127.0 ||
            slotNumber < 0.0 || slotNumber > 255.0)
        {
            wxl::game::Native<luaoff::LuaPushNilFn>(luaoff::kLuaPushNil)(state);
            return 1;
        }

        Variant variant;
        {
            const std::lock_guard lock(g_variantMutex);
            const auto found = g_variants.find(VariantKey(
                static_cast<int8_t>(bagNumber), static_cast<uint8_t>(slotNumber)));
            if (found == g_variants.end())
            {
                wxl::game::Native<luaoff::LuaPushNilFn>(luaoff::kLuaPushNil)(state);
                return 1;
            }
            variant = found->second;
        }

        const auto pushNumber =
            wxl::game::Native<luaoff::LuaPushNumberFn>(luaoff::kLuaPushNumber);
        pushNumber(state, variant.itemId);
        pushNumber(state, variant.modifierId);
        wxl::game::Native<luaoff::LuaPushStringFn>(luaoff::kLuaPushString)(
            state, variant.label.c_str());
        const std::string icon =
            wxl::client::retailitem::VariantIconPath(
                variant.itemId, variant.modifierId);
        if (icon.empty())
            wxl::game::Native<luaoff::LuaPushNilFn>(luaoff::kLuaPushNil)(state);
        else
            wxl::game::Native<luaoff::LuaPushStringFn>(luaoff::kLuaPushString)(
                state, icon.c_str());
        pushNumber(state, variant.sourceItemId);
        pushNumber(state, variant.itemLevel);
        std::string spellEffects;
        for (const SpellEffectOverride& effect :
            variant.spellEffectOverrides)
        {
            const uint32_t source = effect.sourceValue < 0
                ? static_cast<uint32_t>(-int64_t(effect.sourceValue))
                : static_cast<uint32_t>(effect.sourceValue);
            const uint32_t target = effect.targetValue < 0
                ? static_cast<uint32_t>(-int64_t(effect.targetValue))
                : static_cast<uint32_t>(effect.targetValue);
            const std::string pair = std::to_string(source) + "=" +
                std::to_string(target);
            if ((";" + spellEffects + ";").find(";" + pair + ";") !=
                std::string::npos)
                continue;
            if (!spellEffects.empty()) spellEffects += ';';
            spellEffects += pair;
        }
        if (spellEffects.empty())
            wxl::game::Native<luaoff::LuaPushNilFn>(luaoff::kLuaPushNil)(state);
        else
            wxl::game::Native<luaoff::LuaPushStringFn>(luaoff::kLuaPushString)(
                state, spellEffects.c_str());
        return 7;
    }

    int __cdecl LuaItemVariantForItem(void* state)
    {
        const double itemNumber =
            wxl::game::Native<luaoff::LuaToNumberFn>(luaoff::kLuaToNumber)(
                state, 1);
        if (!std::isfinite(itemNumber) || itemNumber <= 0.0 ||
            itemNumber > static_cast<double>(
                (std::numeric_limits<uint32_t>::max)()))
        {
            wxl::game::Native<luaoff::LuaPushNilFn>(luaoff::kLuaPushNil)(state);
            return 1;
        }

        const uint32_t itemId = static_cast<uint32_t>(itemNumber);
        Variant variant;
        bool foundVariant = false;
        {
            const std::lock_guard lock(g_variantMutex);
            for (const auto& [key, candidate] : g_variants)
            {
                (void)key;
                if (candidate.itemId != itemId) continue;
                if (foundVariant &&
                    (variant.modifierId != candidate.modifierId ||
                     variant.label != candidate.label ||
                     variant.sourceItemId != candidate.sourceItemId ||
                     variant.itemLevel != candidate.itemLevel ||
                     variant.spellEffectOverrides !=
                        candidate.spellEffectOverrides))
                {
                    wxl::game::Native<luaoff::LuaPushNilFn>(
                        luaoff::kLuaPushNil)(state);
                    return 1;
                }
                variant = candidate;
                foundVariant = true;
            }
        }
        if (!foundVariant)
        {
            wxl::game::Native<luaoff::LuaPushNilFn>(luaoff::kLuaPushNil)(state);
            return 1;
        }

        const auto pushNumber =
            wxl::game::Native<luaoff::LuaPushNumberFn>(luaoff::kLuaPushNumber);
        pushNumber(state, variant.itemId);
        pushNumber(state, variant.modifierId);
        wxl::game::Native<luaoff::LuaPushStringFn>(luaoff::kLuaPushString)(
            state, variant.label.c_str());
        const std::string icon =
            wxl::client::retailitem::VariantIconPath(
                variant.itemId, variant.modifierId);
        if (icon.empty())
            wxl::game::Native<luaoff::LuaPushNilFn>(luaoff::kLuaPushNil)(state);
        else
            wxl::game::Native<luaoff::LuaPushStringFn>(
                luaoff::kLuaPushString)(state, icon.c_str());
        pushNumber(state, variant.sourceItemId);
        pushNumber(state, variant.itemLevel);
        std::string spellEffects;
        for (const SpellEffectOverride& effect : variant.spellEffectOverrides)
        {
            const uint32_t source = effect.sourceValue < 0
                ? static_cast<uint32_t>(-int64_t(effect.sourceValue))
                : static_cast<uint32_t>(effect.sourceValue);
            const uint32_t target = effect.targetValue < 0
                ? static_cast<uint32_t>(-int64_t(effect.targetValue))
                : static_cast<uint32_t>(effect.targetValue);
            const std::string pair = std::to_string(source) + "=" +
                std::to_string(target);
            if ((";" + spellEffects + ";").find(";" + pair + ";") !=
                std::string::npos)
                continue;
            if (!spellEffects.empty()) spellEffects += ';';
            spellEffects += pair;
        }
        if (spellEffects.empty())
            wxl::game::Native<luaoff::LuaPushNilFn>(luaoff::kLuaPushNil)(state);
        else
            wxl::game::Native<luaoff::LuaPushStringFn>(
                luaoff::kLuaPushString)(state, spellEffects.c_str());
        return 7;
    }

    int __cdecl LuaPreloadRetailItem(void* state)
    {
        const auto toNumber =
            wxl::game::Native<luaoff::LuaToNumberFn>(luaoff::kLuaToNumber);
        const double bagNumber = toNumber(state, 1);
        const double slotNumber = toNumber(state, 2);
        const double itemNumber = toNumber(state, 3);
        uint32_t displayId = 0;

        if (std::isfinite(bagNumber) && std::isfinite(slotNumber) &&
            std::isfinite(itemNumber) &&
            bagNumber >= -128.0 && bagNumber <= 127.0 &&
            slotNumber >= 0.0 && slotNumber <= 255.0 &&
            itemNumber > 0.0 &&
            itemNumber <= static_cast<double>(
                (std::numeric_limits<uint32_t>::max)()))
        {
            const int8_t bag = static_cast<int8_t>(bagNumber);
            const uint8_t slot = static_cast<uint8_t>(slotNumber);
            const uint32_t itemId = static_cast<uint32_t>(itemNumber);
            uint32_t modifierId = 0;
            {
                const std::lock_guard lock(g_variantMutex);
                const auto found = g_variants.find(VariantKey(bag, slot));
                if (found != g_variants.end() &&
                    found->second.itemId == itemId)
                    modifierId = found->second.modifierId;
            }
            wxl::client::retailitem::RequestIconForItem(
                itemId, modifierId);
            if (bag < 0)
            {
                // Equipped slots need their model/material graph. Bag
                // enumeration deliberately requests only the icon so opening
                // a large retail bag cannot recreate the old DB2 hitch.
                displayId =
                    wxl::client::retailitem::RequestDisplayForItem(
                        itemId, modifierId);
            }
            else
            {
                displayId = 1;
            }
        }

        wxl::game::Native<luaoff::LuaPushBooleanFn>(luaoff::kLuaPushBoolean)(
            state, displayId ? 1 : 0);
        return 1;
    }

    int __cdecl LuaRetailIconGeneration(void* state)
    {
        wxl::game::Native<luaoff::LuaPushNumberFn>(luaoff::kLuaPushNumber)(
            state,
            static_cast<double>(
                wxl::client::retailitem::IconGeneration()));
        return 1;
    }

    int __cdecl LuaItemVariantGeneration(void* state)
    {
        wxl::game::Native<luaoff::LuaPushNumberFn>(luaoff::kLuaPushNumber)(
            state, static_cast<double>(
                g_variantGeneration.load(std::memory_order_acquire)));
        return 1;
    }

    bool TryPlayNativeItemEquipSound(uint32_t displayId) noexcept
    {
        __try
        {
            wxl::game::Native<soundoff::PlayItemDisplaySoundFn>(
                soundoff::kPlayItemDisplaySound)(1, displayId);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    int __cdecl LuaPlayRetailEquipSound(void* state)
    {
        const auto toNumber =
            wxl::game::Native<luaoff::LuaToNumberFn>(luaoff::kLuaToNumber);
        const double bagNumber = toNumber(state, 1);
        const double slotNumber = toNumber(state, 2);
        const double itemNumber = toNumber(state, 3);
        bool played = false;

        if (std::isfinite(bagNumber) && std::isfinite(slotNumber) &&
            bagNumber >= -128.0 && bagNumber <= 127.0 &&
            slotNumber >= 0.0 && slotNumber <= 255.0)
        {
            Variant instance;
            bool foundInstance = false;
            {
                const std::lock_guard lock(g_variantMutex);
                const auto found = g_variants.find(VariantKey(
                    static_cast<int8_t>(bagNumber),
                    static_cast<uint8_t>(slotNumber)));
                if (found != g_variants.end())
                {
                    instance = found->second;
                    foundInstance = true;
                }
            }
            if (std::isfinite(itemNumber) && itemNumber > 0.0 &&
                itemNumber <= static_cast<double>(
                    std::numeric_limits<uint32_t>::max()))
            {
                const uint32_t linkedItemId = static_cast<uint32_t>(itemNumber);
                // The server snapshot still describes the clicked bag slot
                // until the inventory refresh arrives. Prefer it over a
                // post-action link, which may already describe the item that
                // was swapped back into that slot.
                if (!foundInstance)
                {
                    if (wxl::client::retailitem::IsNativePresentation(
                            linkedItemId))
                    {
                        wxl::game::Native<luaoff::LuaPushBooleanFn>(
                            luaoff::kLuaPushBoolean)(state, 0);
                        return 1;
                    }
                    instance = {};
                    instance.itemId = linkedItemId;
                    foundInstance = true;
                }
            }

            const auto catalog = wxl::runtime::db2::retailitems::Current();
            if (foundInstance && catalog)
            {
                const auto base = catalog->items.find(instance.itemId);
                uint32_t displayId =
                    base == catalog->items.end() ? 0u : base->second.displayId;
                if (instance.modifierId)
                {
                    const uint64_t key =
                        (static_cast<uint64_t>(instance.itemId) << 32) |
                        instance.modifierId;
                    const auto variant = catalog->variants.find(key);
                    if (variant != catalog->variants.end() &&
                        variant->second.displayId)
                        displayId = variant->second.displayId;
                }

                // InventoryType zero is not equippable. Do not add an equip sound
                // to retail consumables merely because UseContainerItem was used.
                if (base != catalog->items.end() &&
                    base->second.inventoryType != 0 && displayId != 0)
                {
                    const uint32_t equipmentSlot =
                        EquipmentSlotForInventoryType(
                            base->second.inventoryType);
                    if (equipmentSlot)
                    {
                        bool overrideChanged = false;
                        {
                            const std::lock_guard lock(g_variantMutex);
                            EquippedOverride& pending =
                                g_equippedOverrides[equipmentSlot];
                            overrideChanged =
                                !pending.active ||
                                pending.variant.itemId != instance.itemId ||
                                pending.variant.modifierId !=
                                    instance.modifierId;
                            pending.variant = instance;
                            pending.createdAtMs = GetTickCount();
                            pending.active = true;
                        }
                        if (overrideChanged)
                        {
                            g_variantGeneration.fetch_add(
                                1, std::memory_order_release);
                            WLOG_INFO(
                                "wxlwow: pending equipped variant slot=%u item=%u modifier=%u display=%u",
                                equipmentSlot, instance.itemId,
                                instance.modifierId, displayId);
                        }
                    }

                    if (TryPlayNativeItemEquipSound(displayId))
                    {
                        played = true;

                        static uint32_t logged = 0;
                        if (logged++ < 16)
                            WLOG_INFO(
                                "wxlwow: native retail equip sound item=%u modifier=%u display=%u",
                                instance.itemId, instance.modifierId, displayId);
                    }
                    else
                        WLOG_WARN(
                            "wxlwow: native retail equip sound fault item=%u display=%u",
                            instance.itemId, displayId);
                }
            }
        }

        wxl::game::Native<luaoff::LuaPushBooleanFn>(luaoff::kLuaPushBoolean)(
            state, played ? 1 : 0);
        return 1;
    }

    int __cdecl LuaCollectionPreviewReset(void* state)
    {
        const double raw = wxl::game::Native<luaoff::LuaToNumberFn>(
            luaoff::kLuaToNumber)(state, 1);
        wxl_modern_m2::SetCollectionPreviewReset(
            std::isfinite(raw) && raw != 0.0);
        return 0;
    }

    constexpr char kFrameScriptBootstrap[] = R"lua(
do
    wxlwow = wxlwow or {}
    wxlwow.request_item_variants = _WXLWOW_REQUEST_ITEM_VARIANTS
    wxlwow.item_variant = _WXLWOW_ITEM_VARIANT
    wxlwow.item_variant_for_item = _WXLWOW_ITEM_VARIANT_FOR_ITEM
    wxlwow.preload_retail_item = _WXLWOW_PRELOAD_RETAIL_ITEM
    wxlwow.retail_icon_generation = _WXLWOW_RETAIL_ICON_GENERATION
    wxlwow.item_variant_generation = _WXLWOW_ITEM_VARIANT_GENERATION
    wxlwow.play_retail_equip_sound = _WXLWOW_PLAY_RETAIL_EQUIP_SOUND
    wxlwow.collection_preview_reset = _WXLWOW_COLLECTION_PREVIEW_RESET
    _WXLWOW_REQUEST_ITEM_VARIANTS = nil
    _WXLWOW_ITEM_VARIANT = nil
    _WXLWOW_ITEM_VARIANT_FOR_ITEM = nil
    _WXLWOW_PRELOAD_RETAIL_ITEM = nil
    _WXLWOW_RETAIL_ICON_GENERATION = nil
    _WXLWOW_ITEM_VARIANT_GENERATION = nil
    _WXLWOW_PLAY_RETAIL_EQUIP_SOUND = nil
    _WXLWOW_COLLECTION_PREVIEW_RESET = nil

    local function itemIDFromLink(link)
        return tonumber(link and string.match(link, "item:(%d+)"))
    end
    local function preloadEquipment()
        if type(wxlwow.preload_retail_item) ~= "function" then return end
        if type(GetInventoryItemLink) == "function" then
            for slot = 1, 19 do
                local itemID = itemIDFromLink(
                    GetInventoryItemLink("player", slot))
                if itemID then
                    wxlwow.preload_retail_item(-1, slot, itemID)
                end
            end
        end
    end
    local function preloadBag(bag)
        if type(wxlwow.preload_retail_item) ~= "function" or
           type(GetContainerNumSlots) ~= "function" or
           type(GetContainerItemLink) ~= "function" then return end
        local bridgeBag = bag < 0 and 127 or bag
        local slots = GetContainerNumSlots(bag) or 0
        for slot = 1, slots do
            local itemID = itemIDFromLink(GetContainerItemLink(bag, slot))
            if itemID then
                wxlwow.preload_retail_item(bridgeBag, slot, itemID)
            end
        end
    end
    local function visibleContainerSignature(preload)
        local ids = {}
        local seen = {}
        local frameCount = NUM_CONTAINER_FRAMES or 13
        for index = 1, frameCount do
            local frame = _G["ContainerFrame" .. index]
            if frame and frame.IsShown and frame:IsShown() and frame.GetID then
                local bag = frame:GetID()
                if type(bag) == "number" and not seen[bag] then
                    seen[bag] = true
                    ids[#ids + 1] = bag
                    if preload then preloadBag(bag) end
                end
            end
        end
        table.sort(ids)
        return table.concat(ids, ",")
    end
    local function fontStringHeight(fontString)
        if fontString.GetStringHeight then
            return fontString:GetStringHeight() or 0
        end
        if fontString.GetHeight then return fontString:GetHeight() or 0 end
        return 0
    end
    local function hideVariantMetadata(tooltip)
        if tooltip.WXLWOWMythicLine then
            tooltip.WXLWOWMythicLine:Hide()
        end
        if tooltip.WXLWOWItemLevelLine then
            tooltip.WXLWOWItemLevelLine:Hide()
        end
    end
    local function prepareVariantMetadataLine(tooltip, bodyLine, field)
        local line = tooltip[field]
        if not line then
            line = tooltip:CreateFontString(nil, "OVERLAY")
            tooltip[field] = line
        end
        if bodyLine.GetFontObject and line.SetFontObject then
            local fontObject = bodyLine:GetFontObject()
            if fontObject then line:SetFontObject(fontObject) end
        elseif bodyLine.GetFont and line.SetFont then
            local fontPath, fontSize, fontFlags = bodyLine:GetFont()
            if fontPath and fontSize then
                line:SetFont(fontPath, fontSize, fontFlags)
            end
        end
        line:SetJustifyH("LEFT")
        line:SetShadowColor(0, 0, 0, 1)
        line:SetShadowOffset(1, -1)
        return line
    end
    local function applyScaledSpellEffects(
        tooltip, tooltipName, spellEffectAmounts)
        if not spellEffectAmounts or spellEffectAmounts == "" or
           not tooltip.NumLines then return end

        local replacements = {}
        for source, target in string.gmatch(
            spellEffectAmounts, "(%d+)=(%d+)") do
            replacements[#replacements + 1] = { source, target }
        end
        if #replacements == 0 then return end

        for index = 2, tooltip:NumLines() do
            local line = _G[tooltipName .. "TextLeft" .. index]
            local value = line and line:GetText()
            if value and
               (string.find(value, "^Use:") or
                string.find(value, "^Equip:") or
                string.find(value, "^Chance on hit:")) then
                for _, replacement in ipairs(replacements) do
                    -- Replace only the first standalone occurrence. The
                    -- effect amount precedes duration/cooldown text in native
                    -- item spell descriptions, so an equal duration remains
                    -- unchanged.
                    value = string.gsub(value,
                        "%f[%d]" .. replacement[1] .. "%f[%D]",
                        replacement[2], 1)
                end
                line:SetText(value)
            end
        end
    end
    local function applyVariantLine(tooltip, currentItemID, itemID,
        modifierID, label, variantIcon, sourceItemID, itemLevel,
        spellEffectAmounts)
        if not itemID or not label or label == "" or
           (currentItemID and currentItemID ~= itemID) then return end

        local tooltipName = tooltip.GetName and tooltip:GetName()
        if not tooltipName then return end
        local nameLine = _G[tooltipName .. "TextLeft1"]
        if not nameLine then return end

        local sourceName
        if sourceItemID and sourceItemID > 0 and
           type(GetItemInfo) == "function" then
            sourceName = GetItemInfo(sourceItemID)
        end
        local baseName = sourceName or nameLine:GetText() or ""
        -- Never carry metadata from a previous tooltip presentation forward.
        baseName = string.match(baseName, "^[^\n]*") or baseName
        label = string.gsub(label, "^Mythic%s*%+%s*", "Mythic ")

        local oldNameHeight = fontStringHeight(nameLine)
        local metadataLineCount = 1
        if itemLevel and itemLevel > 0 then
            metadataLineCount = 2
        end
        -- Real spaces keep the blank lines measurable on older FrameXML
        -- builds while separate FontStrings provide body-sized metadata.
        nameLine:SetText(baseName .. string.rep("\n ", metadataLineCount))

        local bodyLine = _G[tooltipName .. "TextLeft2"] or nameLine
        local mythicLine = prepareVariantMetadataLine(
            tooltip, bodyLine, "WXLWOWMythicLine")
        local itemLevelLine = prepareVariantMetadataLine(
            tooltip, bodyLine, "WXLWOWItemLevelLine")
        local _, nameFontSize = nameLine:GetFont()
        local _, bodyFontSize = bodyLine:GetFont()
        local firstLineOffset = (nameFontSize or bodyFontSize or 12) + 1

        mythicLine:ClearAllPoints()
        mythicLine:SetPoint(
            "TOPLEFT", nameLine, "TOPLEFT", 0, -firstLineOffset)
        mythicLine:SetText(label)
        mythicLine:SetTextColor(0.0, 1.0, 0.0)
        mythicLine:Show()

        itemLevelLine:ClearAllPoints()
        if itemLevel and itemLevel > 0 then
            itemLevelLine:SetPoint(
                "TOPLEFT", mythicLine, "BOTTOMLEFT", 0, -1)
            itemLevelLine:SetText("Item Level " .. itemLevel)
            itemLevelLine:SetTextColor(1.0, 0.82, 0.0)
            itemLevelLine:Show()
        else
            itemLevelLine:Hide()
        end

        -- The native rows, money frames and socket textures stay attached to
        -- their original FontStrings. Expanding only the name FontString opens
        -- the metadata gap without copying content into differently-indented
        -- native rows.
        local addedHeight = fontStringHeight(nameLine) - oldNameHeight
        if addedHeight <= 0 and nameLine.GetFont then
            local _, fontSize = nameLine:GetFont()
            addedHeight = metadataLineCount * (fontSize or 12)
        end
        if addedHeight > 0 and tooltip.GetHeight and tooltip.SetHeight then
            tooltip:SetHeight(tooltip:GetHeight() + addedHeight)
        end
        applyScaledSpellEffects(
            tooltip, tooltipName, spellEffectAmounts)
        tooltip:Show()
    end

    local function addVariantLine(tooltip, bag, slot, currentItemID)
        local itemID, modifierID, label, variantIcon, sourceItemID, itemLevel,
            spellEffectAmounts = wxlwow.item_variant(bag, slot)
        applyVariantLine(tooltip, currentItemID, itemID, modifierID, label,
            variantIcon, sourceItemID, itemLevel, spellEffectAmounts)
    end

    local function linkedVariantPayload(itemID)
        if not itemID or type(wxlwow.item_variant_for_item) ~= "function" then
            return nil
        end
        local variantID, modifierID, label, variantIcon, sourceItemID,
            itemLevel, spellEffects = wxlwow.item_variant_for_item(itemID)
        if not variantID then return nil end
        label = string.gsub(label or "", "^Mythic%s*%+?%s*", "")
        local level = tonumber(label) or 0
        if level <= 0 then return nil end
        return string.format("%d:%d:%d:%d:%d:%s", variantID,
            modifierID or 0, sourceItemID or 0, itemLevel or 0, level,
            spellEffects or "")
    end

    if type(ChatEdit_InsertLink) == "function" and
       not _G.WXLWOW_MythicRewardLinkWrapped then
        _G.WXLWOW_MythicRewardLinkWrapped = true
        local originalChatEditInsertLink = ChatEdit_InsertLink
        ChatEdit_InsertLink = function(link, ...)
            local itemID = itemIDFromLink(link)
            local payload = linkedVariantPayload(itemID)
            if payload then
                local color, text = string.match(link or "",
                    "^(|c%x%x%x%x%x%x%x%x).*|h(%b[])|h|r$")
                if not text then
                    text = string.match(link or "", "|h(%b[])|h") or
                        "[Mythic Reward]"
                end
                link = (color or "|cffa335ee") ..
                    "|Hwxlmythicitem:" .. payload .. "|h" .. text .. "|h|r"
            end
            return originalChatEditInsertLink(link, ...)
        end
    end

    if type(SetItemRef) == "function" and
       not _G.WXLWOW_MythicRewardReferenceWrapped then
        _G.WXLWOW_MythicRewardReferenceWrapped = true
        local originalSetItemRef = SetItemRef
        local function applyLinkedVariantTooltip(tooltip)
            local data = tooltip and tooltip.WXLWOWLinkedVariant
            if not data then return end
            local key = tostring(data.itemID) .. ":" ..
                tostring(data.level) .. ":" .. tostring(data.itemLevel)
            if tooltip.WXLWOWAppliedLinkedKey == key then return end
            tooltip.WXLWOWAppliedLinkedKey = key
            applyVariantLine(tooltip, data.itemID, data.itemID,
                data.modifierID, "Mythic " .. data.level, nil,
                data.sourceItemID, data.itemLevel, data.effects)
        end
        if ItemRefTooltip and ItemRefTooltip.HookScript then
            ItemRefTooltip:HookScript("OnTooltipSetItem", function(self)
                applyLinkedVariantTooltip(self)
            end)
            ItemRefTooltip:HookScript("OnTooltipCleared", function(self)
                self.WXLWOWAppliedLinkedKey = nil
            end)
        end
        SetItemRef = function(link, text, button)
            local itemID, modifierID, sourceItemID, itemLevel, level, effects =
                string.match(link or "",
                    "^wxlmythicitem:(%d+):(%d+):(%d+):(%d+):(%d+):(.*)$")
            if not itemID then
                if ItemRefTooltip then
                    ItemRefTooltip.WXLWOWLinkedVariant = nil
                    ItemRefTooltip.WXLWOWAppliedLinkedKey = nil
                    hideVariantMetadata(ItemRefTooltip)
                end
                return originalSetItemRef(link, text, button)
            end
            itemID = tonumber(itemID)
            if ItemRefTooltip then
                ItemRefTooltip.WXLWOWAppliedLinkedKey = nil
                ItemRefTooltip.WXLWOWLinkedVariant = {
                    itemID = itemID,
                    modifierID = tonumber(modifierID),
                    sourceItemID = tonumber(sourceItemID),
                    itemLevel = tonumber(itemLevel),
                    level = tonumber(level),
                    effects = effects,
                }
            end
            originalSetItemRef("item:" .. itemID .. ":0:0:0:0:0:0:0",
                text, button)
            applyLinkedVariantTooltip(ItemRefTooltip)
        end
    end

    local function installTooltipHooks()
        if GameTooltip and type(GameTooltip.HookScript) == "function" and
           not _G.WXLWOW_RetailMetadataCleanupHooked then
            _G.WXLWOW_RetailMetadataCleanupHooked = true
            GameTooltip:HookScript("OnTooltipCleared", hideVariantMetadata)
            GameTooltip:HookScript("OnHide", hideVariantMetadata)
        end
        if GameTooltip and type(GameTooltip.SetBagItem) == "function" and
           not _G.WXLWOW_RetailBagTooltipWrapped then
            _G.WXLWOW_RetailBagTooltipWrapped = true
            local originalSetBagItem = GameTooltip.SetBagItem
            GameTooltip.SetBagItem = function(self, bag, slot)
                hideVariantMetadata(self)
                local result = originalSetBagItem(self, bag, slot)
                local link = GetContainerItemLink and GetContainerItemLink(bag, slot)
                addVariantLine(self, bag, slot, itemIDFromLink(link))
                return result
            end
        end

        if GameTooltip and type(GameTooltip.SetInventoryItem) == "function" and
           not _G.WXLWOW_RetailInventoryTooltipWrapped then
            _G.WXLWOW_RetailInventoryTooltipWrapped = true
            local originalSetInventoryItem = GameTooltip.SetInventoryItem
            GameTooltip.SetInventoryItem = function(self, unit, slot)
                hideVariantMetadata(self)
                local result = originalSetInventoryItem(self, unit, slot)
                if not UnitIsUnit or UnitIsUnit(unit, "player") then
                    local link = GetInventoryItemLink and GetInventoryItemLink(unit, slot)
                    addVariantLine(self, -1, slot, itemIDFromLink(link))
                end
                return result
            end
        end
    end
    installTooltipHooks()

    local function resolvedVariantIcon(variantIcon, sourceItemID)
        if variantIcon then return variantIcon end
        if sourceItemID and sourceItemID > 0 and
           type(GetItemIcon) == "function" then
            return GetItemIcon(sourceItemID)
        end
        return nil
    end

    if type(GetContainerItemInfo) == "function" and
       not _G.WXLWOW_RetailContainerInfoWrapped then
        _G.WXLWOW_RetailContainerInfoWrapped = true
        local originalGetContainerItemInfo = GetContainerItemInfo
        GetContainerItemInfo = function(bag, slot)
            local texture, count, locked, quality, readable, lootable, link =
                originalGetContainerItemInfo(bag, slot)
            local linkedItemID = itemIDFromLink(link)
            if linkedItemID and
               type(wxlwow.preload_retail_item) == "function" then
                wxlwow.preload_retail_item(bag, slot, linkedItemID)
            end
            local itemID, modifierID, label, variantIcon, sourceItemID =
                wxlwow.item_variant(bag, slot)
            variantIcon = resolvedVariantIcon(variantIcon, sourceItemID)
            if variantIcon and itemID == linkedItemID then
                texture = variantIcon
            end
            return texture, count, locked, quality, readable, lootable, link
        end
    end

    if type(GetInventoryItemTexture) == "function" and
       not _G.WXLWOW_RetailInventoryTextureWrapped then
        _G.WXLWOW_RetailInventoryTextureWrapped = true
        local originalGetInventoryItemTexture = GetInventoryItemTexture
        GetInventoryItemTexture = function(unit, slot)
            local texture = originalGetInventoryItemTexture(unit, slot)
            if not UnitIsUnit or UnitIsUnit(unit, "player") then
                local itemID, modifierID, label, variantIcon, sourceItemID =
                    wxlwow.item_variant(-1, slot)
                variantIcon = resolvedVariantIcon(variantIcon, sourceItemID)
                local link = GetInventoryItemLink and GetInventoryItemLink(unit, slot)
                if variantIcon and itemID == itemIDFromLink(link) then
                    texture = variantIcon
                end
            end
            return texture
        end
    end

    if type(hooksecurefunc) == "function" and
       type(UseContainerItem) == "function" and
       not _G.WXLWOW_RetailEquipSoundHooked then
        _G.WXLWOW_RetailEquipSoundHooked = true
        hooksecurefunc("UseContainerItem", function(bag, slot)
            -- The protected native action has already completed. Its old bag
            -- variant remains in the server snapshot until the inventory
            -- refresh arrives, which is enough to stage the equipped model and
            -- play the compatible sound without tainting UseContainerItem.
            local link = GetContainerItemLink and GetContainerItemLink(bag, slot)
            local retailItemID = itemIDFromLink(link)
            local equipLoc
            if GetItemInfo then
                local _, _, _, _, _, _, _, _, resolvedEquipLoc =
                    GetItemInfo(link)
                equipLoc = resolvedEquipLoc
            end
            -- A directly imported retail item may not have a server variant
            -- record. Pass its ID only while the post-action slot still
            -- resolves to equipment; generated variants use the authoritative
            -- snapshot above even if this link has already moved.
            if retailItemID and equipLoc and equipLoc ~= "" then
                wxlwow.play_retail_equip_sound(bag, slot, retailItemID)
            else
                wxlwow.play_retail_equip_sound(bag, slot)
            end
        end)
    end

    local requestPending = false
    local requestDelay = 0
    local presentationPending = false
    local presentationDelay = 0
    local iconGeneration = wxlwow.retail_icon_generation()
    local variantGeneration = wxlwow.item_variant_generation()
    local visibleBagSignature = ""
    local visibleBagScanDelay = 0
    local visibleBagScanPasses = 0
    local function requestVariants()
        requestPending = true
        requestDelay = 0.15
    end
    local function requestPresentationRefresh(delay)
        -- PLAYER_EQUIPMENT_CHANGED/UNIT_INVENTORY_CHANGED run before an
        -- imported display graph or item-instance variant is necessarily
        -- resident. Coalesce that native event with the later generation
        -- publication and rebuild each preview only once after it settles.
        presentationPending = true
        presentationDelay = delay or 0.20
    end
    local function refreshEquipmentPresentation()
        -- PaperDollFrame only rebuilds this clone for UNIT_MODEL_CHANGED.
        -- A server-authoritative variant can arrive after that event, leaving
        -- an already-open CharacterFrame stale until the player reopens it.
        if CharacterModelFrame and CharacterModelFrame.SetUnit and
           CharacterModelFrame.IsShown and CharacterModelFrame:IsShown() then
            CharacterModelFrame:SetUnit("player")
        end

        -- SetPortraitTexture owns a separate model clone. Refresh the native
        -- player and CharacterFrame targets after the modern equipment tree
        -- is ready so newly equipped helms are included as well.
        if type(SetPortraitTexture) == "function" then
            local seen = {}
            local function refresh(texture)
                if texture and not seen[texture] then
                    seen[texture] = true
                    SetPortraitTexture(texture, "player")
                end
            end
            refresh(PlayerPortrait)
            refresh(PlayerFrame and PlayerFrame.portrait)
            refresh(CharacterFramePortrait)
            refresh(CharacterFrame and CharacterFrame.portrait)
        end
    end
    local watcher = CreateFrame and CreateFrame("Frame")
    if watcher then
        watcher:RegisterEvent("ADDON_LOADED")
        watcher:RegisterEvent("PLAYER_ENTERING_WORLD")
        watcher:RegisterEvent("BAG_UPDATE")
        watcher:RegisterEvent("BANKFRAME_OPENED")
        watcher:RegisterEvent("PLAYERBANKSLOTS_CHANGED")
        watcher:RegisterEvent("UNIT_INVENTORY_CHANGED")
        watcher:RegisterEvent("PLAYER_EQUIPMENT_CHANGED")
        watcher:SetScript("OnEvent", function(self, event, unit)
            if event == "ADDON_LOADED" then installTooltipHooks() end
            if event ~= "UNIT_INVENTORY_CHANGED" or unit == "player" then
                requestVariants()
            end
            if (event == "UNIT_INVENTORY_CHANGED" and unit == "player") or
               event == "PLAYER_EQUIPMENT_CHANGED" then
                requestPresentationRefresh(0.30)
            end
        end)
        watcher:SetScript("OnUpdate", function(self, elapsed)
            local nextIconGeneration =
                wxlwow.retail_icon_generation()
            local nextVariantGeneration =
                wxlwow.item_variant_generation()
            local iconChanged = nextIconGeneration ~= iconGeneration
            local variantChanged = nextVariantGeneration ~= variantGeneration
            if iconChanged or variantChanged then
                iconGeneration = nextIconGeneration
                variantGeneration = nextVariantGeneration
                if type(ContainerFrame_UpdateAll) == "function" then
                    ContainerFrame_UpdateAll()
                end
                if variantChanged then
                    requestPresentationRefresh(0.05)
                end
            end
            if presentationPending then
                presentationDelay = presentationDelay - elapsed
                if presentationDelay <= 0 then
                    presentationPending = false
                    refreshEquipmentPresentation()
                end
            end
            visibleBagScanDelay = visibleBagScanDelay - elapsed
            if visibleBagScanDelay <= 0 then
                visibleBagScanDelay = 0.20
                local signature = visibleContainerSignature(false)
                if signature ~= visibleBagSignature then
                    visibleBagSignature = signature
                    visibleBagScanPasses = signature ~= "" and 6 or 0
                end
                if visibleBagScanPasses > 0 then
                    visibleContainerSignature(true)
                    visibleBagScanPasses = visibleBagScanPasses - 1
                end
            end
            if not requestPending then return end
            requestDelay = requestDelay - elapsed
            if requestDelay > 0 then return end
            requestPending = false
            preloadEquipment()
            -- Do not resolve every carried retail item when entering the
            -- world. On a 32-bit client that eagerly builds a large DB2/model
            -- working set even for closed bags. Visible containers still
            -- resolve below and GetContainerItemInfo preloads individual
            -- items on demand, so icons/equip behavior remain unchanged.
            visibleContainerSignature(true)
            wxlwow.request_item_variants()
        end)
    end
end
)lua";

    bool Install()
    {
        bool ok = true;
        ok &= network::RegisterClientOpcode(
            opcodes::CmsgRetailItemVariants,
            "CMSG_WXL_RETAIL_ITEM_VARIANTS");
        ok &= network::RegisterServerOpcode(
            opcodes::SmsgRetailItemVariants,
            "SMSG_WXL_RETAIL_ITEM_VARIANTS",
            &OnVariantSnapshot);
        ok &= framescript::RegisterFunction(
            "_WXLWOW_REQUEST_ITEM_VARIANTS", &LuaRequestItemVariants);
        ok &= framescript::RegisterFunction(
            "_WXLWOW_ITEM_VARIANT", &LuaItemVariant);
        ok &= framescript::RegisterFunction(
            "_WXLWOW_ITEM_VARIANT_FOR_ITEM", &LuaItemVariantForItem);
        ok &= framescript::RegisterFunction(
            "_WXLWOW_PRELOAD_RETAIL_ITEM", &LuaPreloadRetailItem);
        ok &= framescript::RegisterFunction(
            "_WXLWOW_RETAIL_ICON_GENERATION",
            &LuaRetailIconGeneration);
        ok &= framescript::RegisterFunction(
            "_WXLWOW_ITEM_VARIANT_GENERATION",
            &LuaItemVariantGeneration);
        ok &= framescript::RegisterFunction(
            "_WXLWOW_PLAY_RETAIL_EQUIP_SOUND",
            &LuaPlayRetailEquipSound);
        ok &= framescript::RegisterFunction(
            "_WXLWOW_COLLECTION_PREVIEW_RESET",
            &LuaCollectionPreviewReset);
        ok &= framescript::RegisterScript(
            "retail-item-variants", kFrameScriptBootstrap);
        return ok;
    }
}

namespace
{
    uint32_t __cdecl PresentationSourceApi(uint32_t itemId)
    {
        return wxl::client::wxlwow::PresentationSourceForItem(itemId);
    }

    uint32_t __cdecl VisibleModifierApi(uint32_t itemId)
    {
        return wxl::client::wxlwow::VisibleModifierForItem(itemId);
    }

    int __cdecl EquippedVariantApi(uint32_t equipmentSlot,
                                   uint32_t* itemId, uint32_t* modifierId)
    {
        if (!itemId || !modifierId) return 0;
        return wxl::client::wxlwow::EquippedVariant(
            equipmentSlot, *itemId, *modifierId) ? 1 : 0;
    }

    uint64_t __cdecl VariantGenerationApi()
    {
        return wxl::client::wxlwow::VariantGeneration();
    }
}

bool wxl_modern_m2::InstallItemVariantBridge()
{
    if (!Install())
    {
        WLOG_ERROR("retail item-variant bridge registration failed");
        return false;
    }

    static WXL_ItemVariantApi api = {
        sizeof(WXL_ItemVariantApi),
        WXL_ITEM_VARIANT_API_VERSION,
        &PresentationSourceApi,
        &VisibleModifierApi,
        &EquippedVariantApi,
        &VariantGenerationApi,
    };
    g_api->PublishInterface(
        "wxl.item-variants", WXL_ITEM_VARIANT_API_VERSION, &api);

    WLOG_INFO("retail item-variant/icon bridge active");
    return true;
}

uint32_t wxl::client::wxlwow::VariantGeneration() noexcept
{
    return g_variantGeneration.load(std::memory_order_acquire);
}

bool wxl::client::wxlwow::EquippedVariant(
    uint32_t equipmentSlot, uint32_t& itemId, uint32_t& modifierId) noexcept
{
    itemId = 0;
    modifierId = 0;
    if (!equipmentSlot || equipmentSlot > 19) return false;

    const std::lock_guard lock(g_variantMutex);
    const EquippedOverride& pending =
        g_equippedOverrides[equipmentSlot];
    if (pending.active && pending.variant.itemId)
    {
        itemId = pending.variant.itemId;
        modifierId = pending.variant.modifierId;
        return true;
    }
    const auto found = g_variants.find(
        VariantKey(-1, static_cast<uint8_t>(equipmentSlot)));
    if (found == g_variants.end() || !found->second.itemId) return false;
    itemId = found->second.itemId;
    modifierId = found->second.modifierId;
    return true;
}

uint32_t wxl::client::wxlwow::VisibleModifierForItem(
    uint32_t itemId) noexcept
{
    if (!itemId) return 0;
    const std::lock_guard lock(g_variantMutex);

    for (uint32_t slot = 1;
         slot < g_equippedOverrides.size(); ++slot)
    {
        const EquippedOverride& pending = g_equippedOverrides[slot];
        if (pending.active && pending.variant.itemId == itemId)
            return pending.variant.modifierId;
    }

    uint32_t candidate = 0;
    for (const auto& [key, value] : g_variants)
    {
        if (value.itemId != itemId || !value.modifierId) continue;
        if (static_cast<int8_t>(key >> 8) == -1)
            return value.modifierId;
        if (!candidate)
            candidate = value.modifierId;
        else if (candidate != value.modifierId)
            return 0;
    }
    return candidate;
}

uint32_t wxl::client::wxlwow::PresentationSourceForItem(
    uint32_t itemId) noexcept
{
    if (!itemId) return 0;
    const std::lock_guard lock(g_variantMutex);

    uint32_t sourceItemId = 0;
    const auto consider = [&](const Variant& variant) -> bool
    {
        if (variant.itemId != itemId || !variant.sourceItemId)
            return true;
        if (!sourceItemId)
            sourceItemId = variant.sourceItemId;
        return sourceItemId == variant.sourceItemId;
    };

    for (const EquippedOverride& pending : g_equippedOverrides)
        if (pending.active && !consider(pending.variant))
            return 0;
    for (const auto& [key, variant] : g_variants)
    {
        (void)key;
        if (!consider(variant)) return 0;
    }
    return sourceItemId;
}
