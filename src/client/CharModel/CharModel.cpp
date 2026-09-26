// Character-model equipment detours: publish item slot change/clear events around the native slot handlers.
// Copyright (C) 2026 WarcraftXL
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

#include "client/Item/RetailItem.hpp"
#include "client/PrivateClientOffsets.hpp"
#include "ExtensionApi.hpp"
#include "engine/assets/db2/ItemDisplayIndex.hpp"
#include "engine/assets/db2/RetailItemCatalog.hpp"
#include "engine/events/Event.hpp"

#include "offsets/game/DB2.hpp"
#include "offsets/game/M2.hpp"
#include "offsets/game/Unit.hpp"
#include "offsets/game/World.hpp"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <intrin.h>
#include <vector>

namespace
{
    namespace ev = wxl::events;
    namespace itemdisplay = wxl::runtime::db2::itemdisplay;
    namespace retailitems = wxl::runtime::db2::retailitems;
    namespace db2 = wxl::offsets::game::db2;
    namespace m2 = wxl::offsets::game::m2;
    namespace private_m2 = wxl_modern_m2::private_offsets::m2;
    namespace private_unit = wxl_modern_m2::private_offsets::unit;
    namespace world = wxl::offsets::game::world;

    constexpr uint32_t kGlueEquipmentBatchTimeoutMs = 5000;

    private_m2::ApplyDisplayFn g_origApplyDisplay = nullptr;
    private_m2::CharacterRemoveVisualsFn g_origCharacterRemoveVisuals = nullptr;
    private_m2::GlueSelectCharacterFn g_origGlueSelectCharacter = nullptr;
    private_unit::FieldSetWriteFn g_origUnitFieldSetWrite = nullptr;
    bool g_collectionPreviewReset = false;

    bool NativeDisplayExists(uint32_t displayId) noexcept
    {
        if (!displayId) return false;
        __try
        {
            const uint32_t minId =
                *reinterpret_cast<const uint32_t*>(db2::itemdisplayinfo::kMinId);
            const uint32_t maxId =
                *reinterpret_cast<const uint32_t*>(db2::itemdisplayinfo::kMaxId);
            void** table =
                *reinterpret_cast<void***>(db2::itemdisplayinfo::kIdTable);
            return table && displayId >= minId && displayId <= maxId &&
                   table[displayId - minId] != nullptr;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    void QueueRetailDisplay(uint32_t displayId)
    {
        if (NativeDisplayExists(displayId)) return;

        const auto catalog = retailitems::Current();
        if (!catalog || !catalog->displayIds.contains(displayId)) return;

        itemdisplay::Request(displayId);
    }

    bool IsDisplayResolved(uint32_t displayId,
                           const std::shared_ptr<const itemdisplay::Index>& index)
    {
        return NativeDisplayExists(displayId) ||
               (index && index->resolvedDisplays.contains(displayId));
    }

    bool IsCharacterCreationActive() noexcept
    {
        __try
        {
            return *reinterpret_cast<const int32_t*>(world::kCurrentMapId) < 0 &&
                   *reinterpret_cast<void* const*>(
                       private_m2::kCharacterCreationComponent) != nullptr;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // Matches the native starting-outfit scan at 0x4E11D6..0x4E1253.
    uint32_t CopyOutfitDisplays(const uint8_t* rows, uint32_t count,
                               uint32_t race, uint32_t gender, uint32_t classId,
                               uint32_t* out)
    {
        for (uint32_t i = 0; i < count; ++i)
        {
            const auto* row = rows + i * 0x128;
            if (row[4] != race || row[5] != classId || row[6] != gender) continue;
            uint32_t used = 0;
            for (uint32_t slot = 0; slot < 24; ++slot)
            {
                const int32_t display = *reinterpret_cast<const int32_t*>(row + 0x68 + slot * 4);
                const uint32_t inventory = *reinterpret_cast<const uint32_t*>(row + 0xC8 + slot * 4);
                if (display <= 0 || inventory == 1) continue; // native creation skips helmets
                bool duplicate = false;
                for (uint32_t j = 0; j < used; ++j) duplicate |= out[j] == static_cast<uint32_t>(display);
                if (!duplicate) out[used++] = static_cast<uint32_t>(display);
            }
            return used; // native uses the first matching row
        }
        return 0;
    }

    uint32_t ReadCreationOutfit(uint32_t* displays) noexcept
    {
        __try
        {
            const auto* cmo = *reinterpret_cast<const uint8_t* const*>(private_m2::kCharacterCreationComponent);
            const auto* rows = *reinterpret_cast<const uint8_t* const*>(0x00AD336C);
            const uint32_t count = *reinterpret_cast<const uint32_t*>(0x00AD3358);
            if (!cmo || !rows || count > 100000) return 0;
            return CopyOutfitDisplays(rows, count,
                *reinterpret_cast<const uint32_t*>(cmo + 0x18),
                *reinterpret_cast<const uint32_t*>(cmo + 0x1C),
                *reinterpret_cast<const uint32_t*>(0x00AC4220), displays);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    }

    void PrefetchCharacterCreationDisplay(uint32_t displayId)
    {
        if (!displayId || NativeDisplayExists(displayId)) return;
        if (IsDisplayResolved(displayId, itemdisplay::Current())) return;
        uint32_t displays[24]{};
        const uint32_t count = ReadCreationOutfit(displays);
        // Queue every item before waiting: the DB2 worker can resolve one burst
        // instead of incurring its batch latency separately for every slot.
        for (uint32_t i = 0; i < count; ++i)
            wxl::client::retailitem::RequestCharacterCreationDisplay(displays[i]);
        if (!wxl::client::retailitem::RequestCharacterCreationDisplay(displayId)) return;

        const uint32_t started = GetTickCount();
        itemdisplay::Request(displayId);
        while (!IsDisplayResolved(displayId, itemdisplay::Current()) &&
               GetTickCount() - started < kGlueEquipmentBatchTimeoutMs)
            Sleep(5);

        const uint32_t waited = GetTickCount() - started;
        if (IsDisplayResolved(displayId, itemdisplay::Current()))
            WLOG_INFO(
                "charmodel: character-creation display ready display=%u waited=%u ms queued_outfit=%u",
                displayId, waited, count);
        else
            WLOG_WARN(
                "charmodel: character-creation display timed out display=%u waited=%u ms",
                displayId, waited);
    }

    bool ReadSelectedGlueDisplays(uint32_t* displays, uint32_t capacity,
                                  uint32_t* countOut) noexcept
    {
        if (!displays || !countOut || capacity < private_m2::kGlueDisplayCount)
            return false;
        *countOut = 0;
        __try
        {
            const int32_t selected =
                *reinterpret_cast<const int32_t*>(private_m2::kGlueSelectedCharacter);
            const int32_t count =
                *reinterpret_cast<const int32_t*>(private_m2::kGlueCharacterCount);
            const uintptr_t records =
                *reinterpret_cast<const uintptr_t*>(private_m2::kGlueCharacterRecords);
            if (selected < 0 || selected >= count || !records) return false;

            const auto* equipped = reinterpret_cast<const uint32_t*>(
                records + static_cast<uintptr_t>(selected) * private_m2::kGlueCharacterStride +
                private_m2::kGlueDisplayArray);
            for (uint32_t slot = 0; slot < private_m2::kGlueDisplayCount; ++slot)
                displays[(*countOut)++] = equipped[slot];
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    void PrefetchSelectedGlueEquipment()
    {
        uint32_t rawDisplays[private_m2::kGlueDisplayCount]{};
        uint32_t rawCount = 0;
        if (!ReadSelectedGlueDisplays(
                rawDisplays, private_m2::kGlueDisplayCount, &rawCount))
        {
            WLOG_WARN("charmodel: failed to read selected Glue equipment array");
            return;
        }

        const auto catalog = retailitems::Current();
        if (!catalog) return;

        std::vector<uint32_t> displays;
        displays.reserve(rawCount);
        for (uint32_t slot = 0; slot < rawCount; ++slot)
        {
            const uint32_t displayId = rawDisplays[slot];
            if (!displayId || NativeDisplayExists(displayId) ||
                !catalog->displayIds.contains(displayId))
                continue;
            displays.push_back(displayId);
        }

        std::ranges::sort(displays);
        displays.erase(std::unique(displays.begin(), displays.end()), displays.end());
        if (displays.empty()) return;

        auto ready = [&displays]() {
            const auto current = itemdisplay::Current();
            return std::ranges::all_of(displays, [&](uint32_t displayId) {
                return IsDisplayResolved(displayId, current);
            });
        };
        if (ready()) return;

        const uint32_t started = GetTickCount();
        itemdisplay::RequestBatch(displays);
        while (!ready() && GetTickCount() - started < kGlueEquipmentBatchTimeoutMs)
            Sleep(5);

        const uint32_t waited = GetTickCount() - started;
        if (ready())
            WLOG_INFO("charmodel: Glue equipment batch ready displays=%u waited=%u ms",
                      static_cast<uint32_t>(displays.size()), waited);
        else
            WLOG_WARN("charmodel: Glue equipment batch timed out displays=%u waited=%u ms",
                      static_cast<uint32_t>(displays.size()), waited);
    }

    void __cdecl hkGlueSelectCharacter()
    {
        // Native SelectCharacter is a one-shot body composer. Do not expose a
        // half-built character and repair its slots later: publish every retail
        // display first, then let the stock loop build body textures, collection
        // attachments, and weapons together.
        PrefetchSelectedGlueEquipment();
        wxl_modern_m2::CustomizeNoteSelection();
        g_origGlueSelectCharacter();
    }

    /**
     * @brief Observes display application even when the native WotLK ItemDisplayInfo lookup fails.
     *
     * The native owner remains authoritative and runs first. DB2-only IDs do not reach
     * the stock core slot-dispatch event, so this gives native v1.1 consumers a stable
     * CMO/slot/display boundary
     * without fabricating a partial ItemDisplayInfo row.
     */
    void __fastcall hkApplyDisplay(void* cmo, void* edx, uint32_t modelSlot,
                                   uint32_t displayId, uint32_t postFlag)
    {
        // ToggleDress changes the selected class, which can synchronously re-submit the character's
        // Retail outfit. Suppress those DB2-only displays before the stock body compositor sees them
        // while Hide Gear is active; detaching only their 3D models leaves their torso/leg paint baked
        // into the character sheet.
        if (!NativeDisplayExists(displayId) &&
            wxl_modern_m2::SuppressGlueRetailDisplay(cmo, displayId))
            return;

        // Resolve the selected outfit as a burst before its first native display
        // application. Component aliases must still exist before body composition;
        // keep the per-display wait as a fallback for displays outside that outfit.
        if (IsCharacterCreationActive())
            PrefetchCharacterCreationDisplay(displayId);
        else
            QueueRetailDisplay(displayId);
        wxl_modern_m2::EquipmentModelScope equipmentScope(cmo);
        // Retire Retail ownership before native creation can reuse a freed child address.
        if (NativeDisplayExists(displayId))
            wxl_modern_m2::PrepareNativeEquipmentSlot(cmo, modelSlot);
        g_origApplyDisplay(cmo, edx, modelSlot, displayId, postFlag);
        ev::ItemDisplayApplyArgs a{ cmo, modelSlot, displayId, postFlag };
        wxl_modern_m2::Emit(ev::Event::OnItemDisplayApply, &a);
    }

    void __cdecl hkCharacterModelFrameRemoveVisuals(void* cloneRoot)
    {
        const uintptr_t caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
        const bool retailPreviewClone =
            caller == private_m2::kCharacterModelFrameRemoveVisualsReturnA ||
            caller == private_m2::kCharacterModelFrameRemoveVisualsReturnB ||
            caller == private_m2::kPortraitTextureRemoveVisualsReturn;
        if (retailPreviewClone && cloneRoot && !g_collectionPreviewReset)
        {
            static uint32_t loggedCallers = 0;
            const uint32_t callerBit =
                caller == private_m2::kCharacterModelFrameRemoveVisualsReturnA ? 1u :
                caller == private_m2::kCharacterModelFrameRemoveVisualsReturnB ? 2u : 4u;
            if ((loggedCallers & callerBit) == 0)
            {
                loggedCallers |= callerBit;
                WLOG_INFO(
                    "charmodel-preview: preserving cloned retail visual tree clone=%p caller=%08X",
                    cloneRoot, static_cast<uint32_t>(caller));
            }
            return;
        }
        g_origCharacterRemoveVisuals(cloneRoot);
    }

    bool ResolveWeaponVisualSlot(uint32_t fieldIndex, uint32_t& slotOut)
    {
        switch (fieldIndex)
        {
        case private_unit::kVisibleItemMainhandEntry: slotOut = 0; return true;
        case private_unit::kVisibleItemOffhandEntry:  slotOut = 1; return true;
        case private_unit::kVisibleItemRangedEntry:   slotOut = 2; return true;
        default: return false;
        }
    }

    uint32_t __cdecl OnUnitFieldSetCaptured(
        uint32_t fieldArrayBase, uint32_t fieldIndex, uint32_t value)
    {
        uint32_t slot = 0;
        if (!ResolveWeaponVisualSlot(fieldIndex, slot)) return value;

        uint32_t itemId = value;
        uint32_t modifierId = 0;
        if ((value & 0xC0000000u) == 0x80000000u)
        {
            itemId = value & 0x000FFFFFu;
            modifierId = (value >> 20) & 0x000003FFu;
        }
        const auto* visibleItem = reinterpret_cast<const uint32_t*>(
            fieldArrayBase + fieldIndex * sizeof(uint32_t));
        wxl::client::retailitem::RememberVisibleModifier(
            visibleItem, itemId, modifierId);
        wxl::client::retailitem::RequestDisplayForItem(itemId, modifierId);

        void* unitPtr =
            reinterpret_cast<uint8_t*>(fieldArrayBase) -
            private_unit::kFieldArrayOffset;
        ev::WeaponVisualChangeArgs args{ unitPtr, slot, itemId };
        wxl_modern_m2::Emit(ev::Event::OnWeaponVisualChange, &args);

        static bool logged = false;
        if (!logged)
        {
            logged = true;
            WLOG_INFO(
                "weapon-visual: first field update unit=%p slot=%u item=%u",
                unitPtr, slot, itemId);
        }
        return itemId;
    }

    __declspec(naked) void hkUnitFieldSetWrite()
    {
        __asm
        {
            pushfd
            pushad
            push ecx
            push edx
            push eax
            call OnUnitFieldSetCaptured
            add esp, 12
            // pushad saved ECX at +0x18. Replace only that saved register so
            // the displaced native write receives the untagged item ID while
            // every other register and all flags remain unchanged.
            mov [esp + 18h], eax
            popad
            popfd
            jmp g_origUnitFieldSetWrite
        }
    }

    bool InstallCharModel()
    {
        wxl_modern_m2::HookAttach(
            "CharModelApplyDisplay", private_m2::kCharModelApplyDisplay,
                           &hkApplyDisplay, &g_origApplyDisplay);
        wxl_modern_m2::HookAttach("CharacterModelFrameRemoveVisuals",
                           private_m2::kCharacterRemoveVisuals,
                           &hkCharacterModelFrameRemoveVisuals,
                           &g_origCharacterRemoveVisuals);
        // SelectCharacter is a one-shot composer: late slot replay can attach
        // collection geometry, but it cannot reliably rebuild body layers or
        // the stock weapon owner. Resolve the selected equipment as one batch
        // before native composition. Catalog enumeration is O(1), so this no
        // longer stalls the login response while importing retail metadata.
        if (wxl_modern_m2::ConfigBool("WXL_M2_GLUE_PREFETCH", true))
            wxl_modern_m2::HookAttach("GlueSelectCharacter",
                               private_m2::kGlueSelectCharacter,
                               &hkGlueSelectCharacter,
                               &g_origGlueSelectCharacter);
        else
            WLOG_INFO("charmodel: synchronous Glue equipment prefetch disabled by config");
        // This is an instruction hook rather than a function boundary. The naked stub preserves
        // every register and flag before the trampoline resumes the displaced native write.
        wxl_modern_m2::HookAttach(
            "UnitFieldSetWrite", private_unit::kFieldSetWrite,
            reinterpret_cast<void*>(&hkUnitFieldSetWrite),
            reinterpret_cast<void**>(&g_origUnitFieldSetWrite));
        return true;
    }
}

// Glue assembles its first character models before the normal graphics-device phase. Installing
// these detours at Boot keeps those initial equipment slot dispatches observable; later world
// rebuilds and re-equips continue through the same native owner.
bool wxl_modern_m2::InstallCharModel()
{
    return ::InstallCharModel();
}

void wxl_modern_m2::SetCollectionPreviewReset(bool active) noexcept
{
    g_collectionPreviewReset = active;
}
