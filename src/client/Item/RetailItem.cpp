// DBC-first native retail item accessors for icons, visible metadata, and bag auto-equip.
// Copyright (C) 2026 WarcraftXL. GPLv3.

#include "client/FrameScript/WxlWow.hpp"
#include "client/Item/NativeItemDbc.hpp"
#include "client/Item/RetailItem.hpp"
#include "client/PrivateClientOffsets.hpp"
#include "engine/assets/db2/ItemDisplayIndex.hpp"
#include "engine/assets/db2/RetailItemCatalog.hpp"
#include "ExtensionApi.hpp"
#include "game/Binding.hpp"
#include "offsets/engine/Lua.hpp"
#include "offsets/game/DB2.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifndef WXL_RETAIL_ITEM_HOOK_MASK
#define WXL_RETAIL_ITEM_HOOK_MASK 0x1FFFu
#endif

namespace
{
    namespace catalog = wxl::runtime::db2::retailitems;
    namespace itemdisplay = wxl::runtime::db2::itemdisplay;
    namespace nativeitemdbc = wxl::client::nativeitemdbc;
    namespace db2off = wxl::offsets::game::db2;
    namespace private_db2 = wxl_modern_m2::private_offsets::db2;
    namespace luaoff = wxl::offsets::engine::lua;

    constexpr uintptr_t kInventoryArt = 0x0070A910;
    constexpr uintptr_t kItemInventoryArt = 0x0070AA00;
    constexpr uintptr_t kScriptGetItemIcon = 0x00517020;
    constexpr uintptr_t kGetInventoryType = 0x00707280;
    constexpr uintptr_t kCanGoInSlot = 0x00708500;
    constexpr uintptr_t kVisibleItemGetClassId = 0x00758D30;
    constexpr uintptr_t kVisibleItemGetSubclassId = 0x00758D80;
    constexpr uintptr_t kVisibleItemGetInventoryType = 0x00758DD0;
    constexpr uintptr_t kVisibleItemGetDisplayId = 0x00758E50;
    constexpr uintptr_t kVisibleItemGetMaterial = 0x00758ED0;
    constexpr uintptr_t kVisibleItemGetSheatheType = 0x00758F50;
    constexpr uintptr_t kVisibleItemGetSoundOverride = 0x00758FD0;
    constexpr char kQuestionMarkIcon[] = "INV_Misc_QuestionMark";

    using LookupFn = db2off::itemdisplayinfo::LookupFn;
    using InventoryArtFn = const char* (__cdecl*)(uint32_t displayId);
    using ItemInventoryArtFn = const char* (__fastcall*)(void* item, void* edx);
    using ScriptGetItemIconFn = int (__cdecl*)(void* state);
    using GetInventoryTypeFn = uint32_t (__fastcall*)(void* item, void* edx);
    using CanGoInSlotFn = uint32_t (__fastcall*)(void* item, void* edx,
                                                 uint32_t slot, uint32_t flags);
    using VisibleItemValueFn = uint32_t (__fastcall*)(const uint32_t* visibleItem, void* edx);

    LookupFn g_originalLookup = nullptr;
    InventoryArtFn g_originalInventoryArt = nullptr;
    ItemInventoryArtFn g_originalItemInventoryArt = nullptr;
    ScriptGetItemIconFn g_originalScriptGetItemIcon = nullptr;
    GetInventoryTypeFn g_originalGetInventoryType = nullptr;
    CanGoInSlotFn g_originalCanGoInSlot = nullptr;
    VisibleItemValueFn g_originalVisibleItemGetClassId = nullptr;
    VisibleItemValueFn g_originalVisibleItemGetSubclassId = nullptr;
    VisibleItemValueFn g_originalVisibleItemGetInventoryType = nullptr;
    VisibleItemValueFn g_originalVisibleItemGetDisplayId = nullptr;
    VisibleItemValueFn g_originalVisibleItemGetMaterial = nullptr;
    VisibleItemValueFn g_originalVisibleItemGetSheatheType = nullptr;
    VisibleItemValueFn g_originalVisibleItemGetSoundOverride = nullptr;

    std::mutex g_iconMutex;
    std::condition_variable g_iconReady;
    std::unordered_map<uint32_t, std::string> g_iconNames;
    std::unordered_set<uint32_t> g_iconRequests;
    std::unordered_map<uint32_t, uint32_t> g_iconRetryAfterMs;
    std::once_flag g_iconWorkerOnce;
    std::atomic_uint32_t g_iconGeneration = 0;
    struct NativeRowIndex
    {
        std::mutex mutex;
        const uint8_t* base = nullptr;
        uint32_t count = 0;
        std::unordered_map<uint32_t, const uint8_t*> rows;
    };
    NativeRowIndex g_nativeItemRows;
    NativeRowIndex g_nativeDisplayRows;
    std::atomic_uint32_t g_sparseItemDiagnosticCount = 0;
    std::mutex g_displayStringMutex;
    std::unordered_set<std::string> g_displayStrings;
    std::mutex g_db2DisplayFallbackMutex;
    std::unordered_set<uint32_t> g_db2DisplayFallbacks;
    std::mutex g_visibleModifierMutex;
    std::unordered_map<const uint32_t*, std::pair<uint32_t, uint32_t>>
        g_visibleModifiers;
    std::atomic_uint32_t g_visibleDiagnosticCount = 0;
    std::atomic_uint32_t g_inventoryDiagnosticCount = 0;
    std::atomic_uint32_t g_slotDiagnosticCount = 0;
    std::atomic_uint32_t g_iconMissDiagnosticCount = 0;
    thread_local uint32_t g_displayLookupContext = 0;
    thread_local uint32_t g_displayLookupAtMs = 0;

    DWORD WINAPI IconWorker(LPVOID);

    struct NativeStorageView
    {
        const uint8_t* records = nullptr;
        uint32_t count = 0;
    };

    const uint8_t* FastNativeRecord(
        uint32_t id, uintptr_t minAddress, uintptr_t maxAddress,
        uintptr_t tableAddress) noexcept
    {
        if (!id) return nullptr;
        __try
        {
            const uint32_t minId =
                *reinterpret_cast<const uint32_t*>(minAddress);
            const uint32_t maxId =
                *reinterpret_cast<const uint32_t*>(maxAddress);
            void** table = *reinterpret_cast<void***>(tableAddress);
            return table && id >= minId && id <= maxId
                ? static_cast<const uint8_t*>(table[id - minId])
                : nullptr;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }

    NativeStorageView ReadNativeStorage(
        uintptr_t countAddress, uintptr_t recordsAddress) noexcept
    {
        NativeStorageView view;
        __try
        {
            view.count =
                *reinterpret_cast<const uint32_t*>(countAddress);
            view.records =
                *reinterpret_cast<const uint8_t* const*>(recordsAddress);
            // Reject obviously corrupt metadata before allocating an index.
            if (!view.records || !view.count || view.count > 1000000u)
                view = {};
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            view = {};
        }
        return view;
    }

    bool ReadNativeIds(
        const uint8_t* records, uint32_t count, size_t stride,
        uint32_t* ids) noexcept
    {
        if (!records || !count || !stride || !ids) return false;
        __try
        {
            for (uint32_t row = 0; row < count; ++row)
                ids[row] = *reinterpret_cast<const uint32_t*>(
                    records + static_cast<size_t>(row) * stride);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    const uint8_t* CompactNativeRecord(
        NativeRowIndex& index, const NativeStorageView& view,
        size_t stride, uint32_t id, const char* tableName)
    {
        if (!view.records || !view.count || !id) return nullptr;
        {
            const std::lock_guard lock(index.mutex);
            if (index.base == view.records && index.count == view.count)
            {
                const auto found = index.rows.find(id);
                return found == index.rows.end() ? nullptr : found->second;
            }
        }

        try
        {
            std::vector<uint32_t> ids(view.count);
            if (!ReadNativeIds(
                    view.records, view.count, stride, ids.data()))
                return nullptr;

            const std::lock_guard lock(index.mutex);
            if (index.base != view.records || index.count != view.count)
            {
                index.rows.clear();
                index.rows.reserve(view.count);
                for (uint32_t row = 0; row < view.count; ++row)
                {
                    if (!ids[row]) continue;
                    index.rows.insert_or_assign(
                        ids[row],
                        view.records + static_cast<size_t>(row) * stride);
                }
                index.base = view.records;
                index.count = view.count;
                WLOG_INFO(
                    "retail-item: indexed all native %s rows=%u",
                    tableName, view.count);
            }
            const auto found = index.rows.find(id);
            return found == index.rows.end() ? nullptr : found->second;
        }
        catch (...)
        {
            WLOG_ERROR(
                "retail-item: failed to index native %s rows=%u",
                tableName, view.count);
            return nullptr;
        }
    }

    const uint8_t* NativeItemRecord(uint32_t itemId) noexcept
    {
        if (const uint8_t* record = FastNativeRecord(
                itemId, db2off::item::kMinId, db2off::item::kMaxId,
                db2off::item::kIdTable))
            return record;

        const uint8_t* record = CompactNativeRecord(
            g_nativeItemRows,
            ReadNativeStorage(
                private_db2::item::kRecordCount,
                private_db2::item::kRecordData),
            db2off::item::kRecordSize, itemId, "Item.dbc");
        if (record)
        {
            const uint32_t diagnostic =
                g_sparseItemDiagnosticCount.fetch_add(
                    1, std::memory_order_relaxed);
            if (diagnostic < 64)
                WLOG_INFO(
                    "retail-item: recovered sparse Item.dbc row item=%u",
                    itemId);
        }
        return record;
    }

    bool NativeItemExists(uint32_t itemId) noexcept
    {
        if (NativeItemRecord(itemId)) return true;
        uint32_t displayId = 0;
        return nativeitemdbc::SupplementalItemDisplay(
            itemId, displayId);
    }

    uint32_t NativeItemField(uint32_t itemId, size_t offset) noexcept
    {
        if (offset + sizeof(uint32_t) > db2off::item::kRecordSize) return 0;
        __try
        {
            const uint8_t* record = NativeItemRecord(itemId);
            if (record)
                return *reinterpret_cast<const uint32_t*>(record + offset);
            uint32_t value = 0;
            nativeitemdbc::SupplementalItemField(itemId, offset, value);
            return value;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    uint32_t NativeItemDisplayId(uint32_t itemId) noexcept
    {
        if (const uint8_t* record = NativeItemRecord(itemId))
        {
            __try
            {
                return *reinterpret_cast<const uint32_t*>(
                    record + 0x14);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return 0;
            }
        }
        uint32_t displayId = 0;
        nativeitemdbc::SupplementalItemDisplay(
            itemId, displayId);
        return displayId;
    }

    const uint8_t* NativeDisplayRecord(uint32_t displayId) noexcept
    {
        if (const uint8_t* record = FastNativeRecord(
                displayId, db2off::itemdisplayinfo::kMinId,
                db2off::itemdisplayinfo::kMaxId,
                db2off::itemdisplayinfo::kIdTable))
            return record;
        return CompactNativeRecord(
            g_nativeDisplayRows,
            ReadNativeStorage(
                private_db2::itemdisplayinfo::kRecordCount,
                private_db2::itemdisplayinfo::kRecordData),
            private_db2::itemdisplayinfo::kCompactRecordSize, displayId,
            "ItemDisplayInfo.dbc");
    }

    bool NativeDisplayExists(uint32_t displayId) noexcept
    {
        return NativeDisplayRecord(displayId) != nullptr ||
               nativeitemdbc::SupplementalDisplayExists(displayId);
    }

    const char* NativeRecordString(
        const uint8_t* record, size_t offset) noexcept
    {
        if (!record ||
            offset + sizeof(const char*) >
                private_db2::itemdisplayinfo::kCompactRecordSize)
            return nullptr;
        __try
        {
            return *reinterpret_cast<const char* const*>(record + offset);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }

    uint32_t ItemIdFromObject(void* item) noexcept
    {
        if (!item) return 0;
        __try
        {
            const auto objectData = *reinterpret_cast<const uint8_t* const*>(
                static_cast<const uint8_t*>(item) + 0x08);
            return objectData ? *reinterpret_cast<const uint32_t*>(objectData + 0x0C) : 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    uint32_t PresentationItemId(uint32_t itemId) noexcept
    {
        const uint32_t sourceItemId =
            wxl::client::wxlwow::PresentationSourceForItem(itemId);
        return sourceItemId ? sourceItemId : itemId;
    }

    struct VisibleItemIdentity
    {
        uint32_t itemId = 0;
        uint32_t modifierId = 0;
    };

    uint32_t RememberedVisibleModifier(const uint32_t* visibleItem,
                                       uint32_t itemId) noexcept
    {
        if (!visibleItem || !itemId) return 0;
        const std::lock_guard lock(g_visibleModifierMutex);
        const auto found = g_visibleModifiers.find(visibleItem);
        return found != g_visibleModifiers.end() &&
               found->second.first == itemId
            ? found->second.second : 0;
    }

    VisibleItemIdentity DecodeVisibleItem(const uint32_t* visibleItem) noexcept
    {
        VisibleItemIdentity identity;
        if (!visibleItem) return identity;
        uint32_t encoded = 0;
        __try { encoded = *visibleItem; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return identity; }

        if ((encoded & 0xC0000000u) == 0x80000000u)
        {
            identity.itemId = encoded & 0x000FFFFFu;
            identity.modifierId = (encoded >> 20) & 0x000003FFu;
            return identity;
        }
        const int32_t signedEntry = static_cast<int32_t>(encoded);
        identity.itemId = signedEntry < 0
            ? static_cast<uint32_t>(-static_cast<int64_t>(signedEntry)) : encoded;
        identity.modifierId =
            RememberedVisibleModifier(visibleItem, identity.itemId);
        return identity;
    }

    const catalog::Item* FindRetailItem(
        uint32_t itemId, std::shared_ptr<const catalog::Catalog>& owner)
    {
        owner = catalog::Current();
        if (!owner) return nullptr;
        const auto found = owner->items.find(itemId);
        return found == owner->items.end() ? nullptr : &found->second;
    }

    uint32_t RetailDisplayForItem(uint32_t itemId, uint32_t modifierId) noexcept
    {
        const auto owner = catalog::Current();
        if (!owner) return 0;

        if (modifierId)
        {
            const uint64_t key =
                (static_cast<uint64_t>(itemId) << 32) | modifierId;
            const auto variant = owner->variants.find(key);
            if (variant != owner->variants.end() && variant->second.displayId)
                return variant->second.displayId;
        }

        const auto item = owner->items.find(itemId);
        return item == owner->items.end() ? 0u : item->second.displayId;
    }

    uint32_t RetailIconFileDataIdForItem(
        uint32_t itemId, uint32_t modifierId) noexcept
    {
        const auto owner = catalog::Current();
        if (!owner) return 0;

        if (modifierId)
        {
            const uint64_t key =
                (static_cast<uint64_t>(itemId) << 32) | modifierId;
            const auto variant = owner->variants.find(key);
            if (variant != owner->variants.end() &&
                variant->second.iconFileDataId)
                return variant->second.iconFileDataId;
        }

        const auto item = owner->items.find(itemId);
        return item == owner->items.end()
            ? 0u : item->second.iconFileDataId;
    }

    std::string IconStem(std::string path)
    {
        std::replace(path.begin(), path.end(), '/', '\\');
        const size_t slash = path.find_last_of('\\');
        if (slash != std::string::npos) path.erase(0, slash + 1);
        const size_t dot = path.find_last_of('.');
        if (dot != std::string::npos) path.resize(dot);
        return path;
    }

    void EnsureIconWorker()
    {
        std::call_once(g_iconWorkerOnce, [] {
            HANDLE thread =
                CreateThread(nullptr, 0, &IconWorker, nullptr, 0, nullptr);
            if (thread)
                CloseHandle(thread);
            else
                WLOG_ERROR("retail-item: failed to create icon resolver worker");
        });
    }

    void QueueIconFileDataId(uint32_t fileDataId)
    {
        if (!fileDataId) return;
        EnsureIconWorker();

        bool notify = false;
        {
            const std::lock_guard lock(g_iconMutex);
            if (g_iconNames.contains(fileDataId) ||
                g_iconRequests.contains(fileDataId))
                return;
            if (const auto retry = g_iconRetryAfterMs.find(fileDataId);
                retry != g_iconRetryAfterMs.end())
            {
                if (static_cast<int32_t>(
                        GetTickCount() - retry->second) < 0)
                    return;
                g_iconRetryAfterMs.erase(retry);
            }
            notify = g_iconRequests.insert(fileDataId).second;
        }
        if (notify) g_iconReady.notify_one();
    }

    DWORD WINAPI IconWorker(LPVOID)
    {
        for (;;)
        {
            std::vector<uint32_t> requests;
            {
                std::unique_lock lock(g_iconMutex);
                g_iconReady.wait(
                    lock, [] { return !g_iconRequests.empty(); });
                requests.assign(
                    g_iconRequests.begin(), g_iconRequests.end());
            }

            bool published = false;
            for (uint32_t fileDataId : requests)
            {
                std::string stem;
                if (const char* path = wxl_modern_m2::ResolveTexture(fileDataId);
                    path && *path)
                    stem = IconStem(path);

                const std::lock_guard lock(g_iconMutex);
                if (!stem.empty())
                {
                    published |=
                        g_iconNames.try_emplace(
                            fileDataId, std::move(stem)).second;
                    g_iconRetryAfterMs.erase(fileDataId);
                }
                else
                {
                    // A transport failure must not permanently poison an icon.
                    // Throttle retries so a disconnected host also cannot turn
                    // UI polling into a busy request loop.
                    g_iconRetryAfterMs.insert_or_assign(
                        fileDataId, GetTickCount() + 1000u);
                }
                g_iconRequests.erase(fileDataId);
            }
            if (published)
                g_iconGeneration.fetch_add(
                    1, std::memory_order_release);
        }
    }

    const char* IconForFileDataId(uint32_t fileDataId)
    {
        if (!fileDataId) return nullptr;
        {
            const std::lock_guard lock(g_iconMutex);
            const auto cached = g_iconNames.find(fileDataId);
            if (cached != g_iconNames.end())
                return cached->second.c_str();
        }
        QueueIconFileDataId(fileDataId);
        return nullptr;
    }

    const char* RetailIconForItem(uint32_t itemId)
    {
        return IconForFileDataId(
            RetailIconFileDataIdForItem(itemId, 0));
    }

    const char* RetailIconForDisplay(uint32_t displayId)
    {
        // The full loose DBC is authoritative for rows omitted from the
        // client's compact startup table. Only a genuine DBC miss may fall
        // through to the retail DB2 appearance graph.
        if (const char* dbc =
                nativeitemdbc::SupplementalDisplayIcon(displayId))
            return dbc;

        const auto owner = catalog::Current();
        if (!owner) return nullptr;
        const auto found = owner->iconByDisplay.find(displayId);
        return found == owner->iconByDisplay.end()
            ? nullptr : IconForFileDataId(found->second);
    }

    const char* NativeIconForDisplay(uint32_t displayId) noexcept
    {
        const uint8_t* record = NativeDisplayRecord(displayId);
        if (!record) return nullptr;
        const char* icon =
            g_originalInventoryArt
                ? g_originalInventoryArt(displayId) : nullptr;
        return icon && *icon
            ? icon
            : NativeRecordString(
                record, private_db2::itemdisplayinfo::kOffIcon1);
    }

    void RememberDb2DisplayFallback(uint32_t displayId)
    {
        if (!displayId) return;
        const std::lock_guard lock(g_db2DisplayFallbackMutex);
        g_db2DisplayFallbacks.insert(displayId);
    }

    bool IsDb2DisplayFallback(uint32_t displayId)
    {
        const std::lock_guard lock(g_db2DisplayFallbackMutex);
        return g_db2DisplayFallbacks.contains(displayId);
    }

    bool IsQuestionMarkIcon(const char* icon) noexcept
    {
        if (!icon || !*icon) return true;
        const char* name = icon;
        if (const char* slash = std::strrchr(icon, '\\')) name = slash + 1;
        if (const char* slash = std::strrchr(name, '/')) name = slash + 1;
        size_t length = std::strlen(name);
        if (const char* dot = std::strrchr(name, '.'))
            length = static_cast<size_t>(dot - name);
        return length == std::strlen(kQuestionMarkIcon) &&
               _strnicmp(name, kQuestionMarkIcon, length) == 0;
    }

    uint32_t FillItem(uint32_t id, void* output)
    {
        if (!output) return 0;
        std::shared_ptr<const catalog::Catalog> owner;
        const catalog::Item* item = FindRetailItem(id, owner);
        // Retail consumables such as ensembles intentionally have no wearable
        // display. They still need an Item.dbc facade so the stock client can
        // cache, link and use the item while the icon hook resolves its FDID.
        if (!item) return 0;
        const std::array<uint32_t, 8> record{
            id, item->classId, item->subclassId, item->soundOverride,
            item->material, item->displayId, item->inventoryType, item->sheatheType,
        };
        std::memcpy(output, record.data(), sizeof(record));
        return 1;
    }

    const char* StableDisplayString(const char* value)
    {
        if (!value || !*value) return "";
        const std::lock_guard lock(g_displayStringMutex);
        return g_displayStrings.emplace(value).first->c_str();
    }

    uint32_t FillDisplay(uint32_t id, void* output)
    {
        if (!output) return 0;
        const auto catalogOwner = catalog::Current();
        if (!catalogOwner ||
            (!catalogOwner->displayIds.contains(id) &&
             !IsDb2DisplayFallback(id)))
            return 0;

        auto owner = itemdisplay::Current();
        const itemdisplay::DisplayRecord* resolved = nullptr;
        if (owner)
        {
            const auto found = owner->displayRecords.find(id);
            if (found != owner->displayRecords.end()) resolved = &found->second;
        }
        if (!resolved)
        {
            itemdisplay::Request(id);
            // RetailEquipment observes the owning ApplyDisplay call even when
            // this first lookup returns no row. Returning immediately lets all
            // equipped displays enter the worker queue together; the bounded
            // update path replays the resolved native display afterward in
            // both Glue and the world.
            return 0;
        }

        const itemdisplay::DisplayRecord& value = *resolved;
        auto* record = static_cast<uint8_t*>(output);
        std::memset(record, 0, private_db2::itemdisplayinfo::kCompactRecordSize);
        std::memcpy(record, &id, sizeof(id));

        const char* empty = "";
        for (size_t i = 0; i < value.nativeModelNames.size(); ++i)
        {
            const char* model = StableDisplayString(value.nativeModelNames[i]);
            const char* texture = StableDisplayString(value.nativeModelTextures[i]);
            std::memcpy(record + 0x04 + i * 4, &model, sizeof(model));
            std::memcpy(record + 0x0C + i * 4, &texture, sizeof(texture));
        }
        for (size_t offset : {size_t{0x14}, size_t{0x18}})
            std::memcpy(record + offset, &empty, sizeof(empty));
        for (size_t i = 0; i < 3; ++i)
            std::memcpy(record + 0x1C + i * 4, &value.geosets[i], 4);
        std::memcpy(record + 0x28, &value.flags, 4);
        if (const auto sound = catalogOwner->soundByDisplay.find(id);
            sound != catalogOwner->soundByDisplay.end())
            std::memcpy(record + private_db2::itemdisplayinfo::kOffGroupSound,
                        &sound->second, 4);
        for (size_t i = 0; i < value.helmetVis.size(); ++i)
            std::memcpy(record + 0x34 + i * 4, &value.helmetVis[i], 4);
        for (size_t i = 0; i < value.componentTextures.size(); ++i)
        {
            const char* component = StableDisplayString(value.componentTextures[i]);
            std::memcpy(record + 0x3C + i * 4, &component, sizeof(component));
        }
        std::memcpy(record + 0x5C, &value.itemVisual, 4);
        std::memcpy(record + 0x60, &value.particleColor, 4);
        g_displayLookupContext = id;
        g_displayLookupAtMs = GetTickCount();
        return 1;
    }

    uint32_t __fastcall LookupDetour(void* storage, void* edx, uint32_t id, void* output)
    {
        const uint32_t native = g_originalLookup
            ? g_originalLookup(storage, edx, id, output) : 0;
        if (native) return native;
        if (storage == reinterpret_cast<void*>(db2off::item::kStorageObject))
            return FillItem(id, output);
        if (storage == reinterpret_cast<void*>(db2off::itemdisplayinfo::kStorageObject))
            return FillDisplay(id, output);
        return 0;
    }

    template <size_t NativeOffset, uint32_t catalog::Item::*Member>
    uint32_t VisibleValue(const uint32_t* visibleItem, void* edx,
                          VisibleItemValueFn original)
    {
        const VisibleItemIdentity identity = DecodeVisibleItem(visibleItem);
        const uint32_t presentationItemId =
            PresentationItemId(identity.itemId);
        if (NativeItemExists(presentationItemId))
            return NativeItemField(presentationItemId, NativeOffset);
        std::shared_ptr<const catalog::Catalog> owner;
        if (const catalog::Item* item =
            FindRetailItem(presentationItemId, owner))
            return item->*Member;
        return original ? original(visibleItem, edx) : 0;
    }

    uint32_t __fastcall VisibleClassId(const uint32_t* item, void* edx)
    {
        return VisibleValue<0x04, &catalog::Item::classId>(
            item, edx, g_originalVisibleItemGetClassId);
    }

    uint32_t __fastcall VisibleSubclassId(const uint32_t* item, void* edx)
    {
        return VisibleValue<0x08, &catalog::Item::subclassId>(
            item, edx, g_originalVisibleItemGetSubclassId);
    }

    uint32_t __fastcall VisibleInventoryType(const uint32_t* item, void* edx)
    {
        return VisibleValue<0x18, &catalog::Item::inventoryType>(
            item, edx, g_originalVisibleItemGetInventoryType);
    }

    uint32_t __fastcall VisibleDisplayId(const uint32_t* item, void* edx)
    {
        const VisibleItemIdentity identity = DecodeVisibleItem(item);
        const uint32_t presentationItemId =
            PresentationItemId(identity.itemId);
        if (NativeItemExists(presentationItemId))
        {
            const uint32_t displayId =
                NativeItemDisplayId(presentationItemId);
            if (displayId && !NativeDisplayExists(displayId))
            {
                RememberDb2DisplayFallback(displayId);
                itemdisplay::Request(displayId);
            }
            return displayId;
        }

        const auto owner = catalog::Current();
        if (!owner)
            return g_originalVisibleItemGetDisplayId
                ? g_originalVisibleItemGetDisplayId(item, edx) : 0;

        uint32_t modifierId = identity.modifierId;
        if (!modifierId)
            modifierId =
                wxl::client::wxlwow::VisibleModifierForItem(identity.itemId);

        const uint32_t displayId =
            RetailDisplayForItem(presentationItemId, modifierId);
        if (!displayId)
            return g_originalVisibleItemGetDisplayId
                ? g_originalVisibleItemGetDisplayId(item, edx) : 0;

        wxl::runtime::db2::itemdisplay::Request(displayId);
        const uint32_t diagnostic = g_visibleDiagnosticCount.fetch_add(
            1, std::memory_order_relaxed);
        if (diagnostic < 128)
            WLOG_INFO("retail-item: visible item=%u modifier=%u display=%u",
                      identity.itemId, modifierId, displayId);
        return displayId;
    }

    uint32_t __fastcall VisibleMaterial(const uint32_t* item, void* edx)
    {
        return VisibleValue<0x10, &catalog::Item::material>(
            item, edx, g_originalVisibleItemGetMaterial);
    }

    uint32_t __fastcall VisibleSheatheType(const uint32_t* item, void* edx)
    {
        return VisibleValue<0x1C, &catalog::Item::sheatheType>(
            item, edx, g_originalVisibleItemGetSheatheType);
    }

    uint32_t __fastcall VisibleSoundOverride(const uint32_t* item, void* edx)
    {
        return VisibleValue<0x0C, &catalog::Item::soundOverride>(
            item, edx, g_originalVisibleItemGetSoundOverride);
    }

    uint32_t __fastcall GetInventoryType(void* item, void* edx)
    {
        const uint32_t itemId = ItemIdFromObject(item);
        const uint32_t presentationItemId = PresentationItemId(itemId);
        if (presentationItemId != itemId &&
            NativeItemExists(presentationItemId))
        {
            const uint32_t inventoryType =
                NativeItemField(presentationItemId, 0x18);
            const uint32_t diagnostic = g_inventoryDiagnosticCount.fetch_add(
                1, std::memory_order_relaxed);
            if (diagnostic < 64)
                WLOG_INFO(
                    "retail-item: generated inventory item=%u source=%u type=%u",
                    itemId, presentationItemId, inventoryType);
            return inventoryType;
        }
        if (NativeItemRecord(itemId))
        {
            const uint32_t inventoryType = g_originalGetInventoryType
                ? g_originalGetInventoryType(item, edx) : 0;
            const uint32_t diagnostic = g_inventoryDiagnosticCount.fetch_add(
                1, std::memory_order_relaxed);
            if (diagnostic < 64)
                WLOG_INFO(
                    "retail-item: native inventory item=%u owner=client type=%u",
                    itemId, inventoryType);
            return inventoryType;
        }
        uint32_t supplementalInventoryType = 0;
        if (nativeitemdbc::SupplementalItemField(
                itemId, 0x18, supplementalInventoryType))
        {
            const uint32_t diagnostic = g_inventoryDiagnosticCount.fetch_add(
                1, std::memory_order_relaxed);
            if (diagnostic < 64)
                WLOG_INFO(
                    "retail-item: native inventory item=%u owner=supplemental type=%u",
                    itemId, supplementalInventoryType);
            return supplementalInventoryType;
        }
        std::shared_ptr<const catalog::Catalog> owner;
        if (const catalog::Item* retail =
            FindRetailItem(presentationItemId, owner))
        {
            const uint32_t diagnostic = g_inventoryDiagnosticCount.fetch_add(
                1, std::memory_order_relaxed);
            if (diagnostic < 64)
                WLOG_INFO("retail-item: inventory item=%u type=%u display=%u material=%u soundOverride=%u retailGroupSound=%u",
                          itemId, retail->inventoryType, retail->displayId,
                          retail->material, retail->soundOverride,
                          retail->itemGroupSoundsId);
            return retail->inventoryType;
        }
        return g_originalGetInventoryType ? g_originalGetInventoryType(item, edx) : 0;
    }

    bool InventoryTypeFitsSlot(uint32_t type, uint32_t slot)
    {
        switch (type)
        {
            case 1: return slot == 0; case 2: return slot == 1; case 3: return slot == 2;
            case 4: return slot == 3; case 5: case 20: return slot == 4;
            case 6: return slot == 5; case 7: return slot == 6; case 8: return slot == 7;
            case 9: return slot == 8; case 10: return slot == 9;
            case 11: return slot == 10 || slot == 11;
            case 12: return slot == 12 || slot == 13;
            case 13: return slot == 15 || slot == 16; case 14: return slot == 16;
            case 15: case 25: case 26: case 28: return slot == 17;
            case 16: return slot == 14; case 17: case 21: return slot == 15;
            case 18: case 27: return slot >= 19 && slot <= 22;
            case 19: return slot == 18; case 22: case 23: return slot == 16;
            default: return false;
        }
    }

    uint32_t __fastcall CanGoInSlot(void* item, void* edx, uint32_t slot, uint32_t flags)
    {
        const uint32_t itemId = ItemIdFromObject(item);
        const uint32_t presentationItemId = PresentationItemId(itemId);
        if (presentationItemId != itemId)
        {
            uint32_t inventoryType = 0;
            if (NativeItemExists(presentationItemId))
                inventoryType = NativeItemField(presentationItemId, 0x18);
            else
            {
                std::shared_ptr<const catalog::Catalog> owner;
                if (const catalog::Item* presentation =
                    FindRetailItem(presentationItemId, owner))
                    inventoryType = presentation->inventoryType;
            }
            if (inventoryType)
            {
                const uint32_t allowed =
                    InventoryTypeFitsSlot(inventoryType, slot) ? 1u : 0u;
                const uint32_t diagnostic = g_slotDiagnosticCount.fetch_add(
                    1, std::memory_order_relaxed);
                if (diagnostic < 64)
                    WLOG_INFO(
                        "retail-item: generated slot-check item=%u source=%u type=%u slot=%u allowed=%u",
                        itemId, presentationItemId, inventoryType, slot,
                        allowed);
                return allowed;
            }
        }
        if (NativeItemRecord(itemId))
        {
            const uint32_t allowed = g_originalCanGoInSlot
                ? g_originalCanGoInSlot(item, edx, slot, flags) : 0;
            const uint32_t diagnostic = g_slotDiagnosticCount.fetch_add(
                1, std::memory_order_relaxed);
            if (diagnostic < 64)
                WLOG_INFO(
                    "retail-item: native slot-check item=%u owner=client slot=%u allowed=%u",
                    itemId, slot, allowed);
            return allowed;
        }
        uint32_t supplementalInventoryType = 0;
        if (nativeitemdbc::SupplementalItemField(
                itemId, 0x18, supplementalInventoryType))
        {
            const uint32_t allowed =
                InventoryTypeFitsSlot(supplementalInventoryType, slot) ? 1u : 0u;
            const uint32_t diagnostic = g_slotDiagnosticCount.fetch_add(
                1, std::memory_order_relaxed);
            if (diagnostic < 64)
                WLOG_INFO(
                    "retail-item: native slot-check item=%u owner=supplemental type=%u slot=%u allowed=%u",
                    itemId, supplementalInventoryType, slot, allowed);
            return allowed;
        }
        std::shared_ptr<const catalog::Catalog> owner;
        if (const catalog::Item* retail = FindRetailItem(itemId, owner))
        {
            const uint32_t allowed = InventoryTypeFitsSlot(retail->inventoryType, slot) ? 1u : 0u;
            const uint32_t diagnostic = g_slotDiagnosticCount.fetch_add(
                1, std::memory_order_relaxed);
            if (diagnostic < 64)
                WLOG_INFO("retail-item: slot-check item=%u type=%u slot=%u allowed=%u",
                          itemId, retail->inventoryType, slot, allowed);
            return allowed;
        }
        return g_originalCanGoInSlot ? g_originalCanGoInSlot(item, edx, slot, flags) : 0;
    }

    const char* __cdecl InventoryArt(uint32_t displayId)
    {
        // Calling the native accessor for a retail display re-enters the hooked
        // ItemDisplayInfo lookup and can queue an entire model/material graph.
        // Bags only need the icon, so preserve DBC priority with an O(1) native
        // identity check and answer known retail displays directly.
        if (NativeDisplayExists(displayId))
            return g_originalInventoryArt
                ? g_originalInventoryArt(displayId) : kQuestionMarkIcon;
        if (const auto owner = catalog::Current();
            owner && owner->displayIds.contains(displayId))
        {
            if (const char* retail = RetailIconForDisplay(displayId))
                return retail;
            return kQuestionMarkIcon;
        }
        const char* native =
            g_originalInventoryArt ? g_originalInventoryArt(displayId) : nullptr;
        return native ? native : kQuestionMarkIcon;
    }

    const char* __fastcall ItemInventoryArt(void* item, void* edx)
    {
        const uint32_t requestedItemId = ItemIdFromObject(item);
        const uint32_t itemId = PresentationItemId(requestedItemId);
        const bool nativeItem = NativeItemExists(itemId);
        // The native accessor reads the generated item's own object fields.
        // Once presentation is redirected to a source item, resolve its
        // display row directly instead of asking the placeholder object.
        const char* native = nativeItem && itemId == requestedItemId &&
            g_originalItemInventoryArt
            ? g_originalItemInventoryArt(item, edx) : nullptr;
        if (nativeItem && !IsQuestionMarkIcon(native))
            return native;

        // Some native consumers fail their second lookup when an otherwise
        // valid Item.dbc ID is far outside the stock range. Resolve through
        // the row's display ID directly so every loaded DBC row behaves the
        // same, regardless of item ID.
        const uint32_t nativeDisplayId = NativeItemDisplayId(itemId);
        if (const char* displayIcon = NativeIconForDisplay(nativeDisplayId))
        {
            if (!IsQuestionMarkIcon(displayIcon))
                return displayIcon;
            // A present ItemDisplayInfo.dbc row remains authoritative even
            // when it intentionally names the question-mark icon.
            return native ? native : displayIcon;
        }

        // A server-generated Item.dbc can reference a modern display that is
        // intentionally absent from ItemDisplayInfo.dbc. Only that missing
        // display boundary falls through to the host-owned retail DB2 graph.
        if (nativeDisplayId)
        {
            if (const char* retail = RetailIconForDisplay(nativeDisplayId))
                return retail;
        }

        std::shared_ptr<const catalog::Catalog> owner;
        if (FindRetailItem(itemId, owner))
        {
            if (const char* retail = RetailIconForItem(itemId))
                return retail;
            return kQuestionMarkIcon;
        }

        if (nativeItem)
        {
            const uint32_t diagnostic = g_iconMissDiagnosticCount.fetch_add(
                1, std::memory_order_relaxed);
            if (diagnostic < 64)
                WLOG_WARN(
                    "retail-item: unresolved DBC icon item=%u source=%u display=%u",
                    requestedItemId, itemId, NativeItemDisplayId(itemId));
            return native ? native : kQuestionMarkIcon;
        }

        native = itemId == requestedItemId && g_originalItemInventoryArt
            ? g_originalItemInventoryArt(item, edx) : nullptr;
        return native ? native : kQuestionMarkIcon;
    }

    uint32_t ItemIdFromLua(void* state)
    {
        const auto toNumber = wxl::game::Native<luaoff::LuaToNumberFn>(luaoff::kLuaToNumber);
        const double numeric = toNumber(state, 1);
        if (numeric > 0.0 && numeric <= static_cast<double>((std::numeric_limits<uint32_t>::max)()))
            return static_cast<uint32_t>(numeric);

        const auto toString = wxl::game::Native<luaoff::LuaToStringFn>(luaoff::kLuaToString);
        size_t length = 0;
        const char* text = toString(state, 1, &length);
        if (!text || !length) return 0;
        const char* end = text + length;
        constexpr char needle[] = "item:";
        const char* begin = std::search(text, end, std::begin(needle), std::end(needle) - 1);
        if (begin == end) return 0;
        begin += sizeof(needle) - 1;
        uint64_t value = 0;
        while (begin != end && *begin >= '0' && *begin <= '9')
        {
            value = value * 10u + static_cast<unsigned>(*begin++ - '0');
            if (value > (std::numeric_limits<uint32_t>::max)()) return 0;
        }
        return static_cast<uint32_t>(value);
    }

    int __cdecl ScriptGetItemIcon(void* state)
    {
        const uint32_t requestedItemId = ItemIdFromLua(state);
        const uint32_t itemId = PresentationItemId(requestedItemId);
        const uint32_t nativeDisplayId = NativeItemDisplayId(itemId);
        if (nativeDisplayId && NativeDisplayExists(nativeDisplayId))
        {
            const char* icon = NativeIconForDisplay(nativeDisplayId);
            std::string path = "Interface\\Icons\\";
            path += icon && *icon ? icon : kQuestionMarkIcon;
            wxl::game::Native<luaoff::LuaPushStringFn>(luaoff::kLuaPushString)(
                state, path.c_str());
            return 1;
        }

        // The item row exists but its display does not: resolve the display
        // itself through DB2. This also covers high-ID custom wrapper items
        // which are not rows in retail Item.db2.
        if (nativeDisplayId)
        {
            if (const char* icon = RetailIconForDisplay(nativeDisplayId))
            {
                std::string path = "Interface\\Icons\\";
                path += icon;
                wxl::game::Native<luaoff::LuaPushStringFn>(
                    luaoff::kLuaPushString)(state, path.c_str());
                return 1;
            }
        }

        std::shared_ptr<const catalog::Catalog> owner;
        if (FindRetailItem(itemId, owner))
        {
            std::string path = "Interface\\Icons\\";
            if (const char* icon = RetailIconForItem(itemId))
                path += icon;
            else
                path += kQuestionMarkIcon;
            wxl::game::Native<luaoff::LuaPushStringFn>(luaoff::kLuaPushString)(
                state, path.c_str());
            return 1;
        }
        return g_originalScriptGetItemIcon ? g_originalScriptGetItemIcon(state) : 0;
    }

    bool InstallRetailItemAccessors()
    {
        bool ok = true;
#if (WXL_RETAIL_ITEM_HOOK_MASK & (1u << 0))
        ok &= wxl_modern_m2::HookAttach("RetailItem::Lookup", db2off::itemdisplayinfo::kLookup,
                                 &LookupDetour, &g_originalLookup);
#endif
#if (WXL_RETAIL_ITEM_HOOK_MASK & (1u << 1))
        ok &= wxl_modern_m2::HookAttach("RetailItem::VisibleClass", kVisibleItemGetClassId,
                                 &VisibleClassId, &g_originalVisibleItemGetClassId);
#endif
#if (WXL_RETAIL_ITEM_HOOK_MASK & (1u << 2))
        ok &= wxl_modern_m2::HookAttach("RetailItem::VisibleSubclass", kVisibleItemGetSubclassId,
                                 &VisibleSubclassId, &g_originalVisibleItemGetSubclassId);
#endif
#if (WXL_RETAIL_ITEM_HOOK_MASK & (1u << 3))
        ok &= wxl_modern_m2::HookAttach("RetailItem::VisibleInventory", kVisibleItemGetInventoryType,
                                 &VisibleInventoryType, &g_originalVisibleItemGetInventoryType);
#endif
#if (WXL_RETAIL_ITEM_HOOK_MASK & (1u << 4))
        ok &= wxl_modern_m2::HookAttach("RetailItem::VisibleDisplay", kVisibleItemGetDisplayId,
                                 &VisibleDisplayId, &g_originalVisibleItemGetDisplayId);
#endif
#if (WXL_RETAIL_ITEM_HOOK_MASK & (1u << 5))
        ok &= wxl_modern_m2::HookAttach("RetailItem::VisibleMaterial", kVisibleItemGetMaterial,
                                 &VisibleMaterial, &g_originalVisibleItemGetMaterial);
#endif
#if (WXL_RETAIL_ITEM_HOOK_MASK & (1u << 6))
        ok &= wxl_modern_m2::HookAttach("RetailItem::VisibleSheathe", kVisibleItemGetSheatheType,
                                 &VisibleSheatheType, &g_originalVisibleItemGetSheatheType);
#endif
#if (WXL_RETAIL_ITEM_HOOK_MASK & (1u << 7))
        ok &= wxl_modern_m2::HookAttach("RetailItem::VisibleSound", kVisibleItemGetSoundOverride,
                                 &VisibleSoundOverride, &g_originalVisibleItemGetSoundOverride);
#endif
#if (WXL_RETAIL_ITEM_HOOK_MASK & (1u << 8))
        ok &= wxl_modern_m2::HookAttach("RetailItem::InventoryType", kGetInventoryType,
                                 &GetInventoryType, &g_originalGetInventoryType);
#endif
#if (WXL_RETAIL_ITEM_HOOK_MASK & (1u << 9))
        ok &= wxl_modern_m2::HookAttach("RetailItem::CanGoInSlot", kCanGoInSlot,
                                 &CanGoInSlot, &g_originalCanGoInSlot);
#endif
#if (WXL_RETAIL_ITEM_HOOK_MASK & (1u << 10))
        ok &= wxl_modern_m2::HookAttach("RetailItem::InventoryArt", kInventoryArt,
                                 &InventoryArt, &g_originalInventoryArt);
#endif
#if (WXL_RETAIL_ITEM_HOOK_MASK & (1u << 11))
        ok &= wxl_modern_m2::HookAttach("RetailItem::ItemInventoryArt", kItemInventoryArt,
                                 &ItemInventoryArt, &g_originalItemInventoryArt);
#endif
#if (WXL_RETAIL_ITEM_HOOK_MASK & (1u << 12))
        ok &= wxl_modern_m2::HookAttach("RetailItem::GetItemIcon", kScriptGetItemIcon,
                                 &ScriptGetItemIcon, &g_originalScriptGetItemIcon);
#endif
        if (ok) WLOG_INFO("retail-item: DBC-first accessors registered");
        return ok;
    }
}

namespace wxl::client::retailitem
{
    bool IsNativePresentation(uint32_t itemId) noexcept
    {
        if (!NativeItemExists(itemId)) return false;
        const uint32_t displayId = NativeItemDisplayId(itemId);
        return !displayId || NativeDisplayExists(displayId);
    }

    void RememberVisibleModifier(const uint32_t* visibleItem,
                                 uint32_t itemId, uint32_t modifierId)
    {
        if (!visibleItem) return;
        const std::lock_guard lock(g_visibleModifierMutex);
        if (!itemId || !modifierId)
            g_visibleModifiers.erase(visibleItem);
        else
            g_visibleModifiers[visibleItem] = {itemId, modifierId};
    }

    bool RequestCharacterCreationDisplay(uint32_t displayId)
    {
        if (!displayId || displayId == UINT32_MAX || NativeDisplayExists(displayId))
            return false;
        // CharStartOutfit is already an explicit appearance reference. Requiring
        // a current Item.db2 owner drops retired heritage appearances before
        // their component textures and collection models can be resolved.
        RememberDb2DisplayFallback(displayId);
        itemdisplay::Request(displayId);
        return true;
    }

    uint32_t RequestDisplayForItem(uint32_t itemId,
                                   uint32_t modifierId) noexcept
    {
        if (!itemId) return 0;
        itemId = PresentationItemId(itemId);

        // A complete DBC pair belongs wholly to WoW's native equipment path.
        // Returning its display here made same-ID retail rows look like
        // variants and allowed the asynchronous controller to replace native
        // WotLK attachments during PLAYER_ENTERING_WORLD.
        if (IsNativePresentation(itemId)) return 0;

        uint32_t displayId = 0;
        if (NativeItemExists(itemId))
            displayId = NativeItemDisplayId(itemId);
        else
            displayId = RetailDisplayForItem(itemId, modifierId);
        if (displayId && !NativeDisplayExists(displayId))
        {
            RememberDb2DisplayFallback(displayId);
            itemdisplay::Request(displayId);
        }
        return displayId;
    }

    void RequestIconForItem(uint32_t itemId,
                            uint32_t modifierId) noexcept
    {
        itemId = PresentationItemId(itemId);
        const uint32_t nativeDisplayId = NativeItemDisplayId(itemId);
        if (nativeDisplayId)
        {
            if (NativeDisplayExists(nativeDisplayId))
                return;
            const auto owner = catalog::Current();
            if (owner)
            {
                const auto found = owner->iconByDisplay.find(nativeDisplayId);
                if (found != owner->iconByDisplay.end())
                {
                    QueueIconFileDataId(found->second);
                    return;
                }
            }
        }
        QueueIconFileDataId(
            RetailIconFileDataIdForItem(itemId, modifierId));
    }

    uint32_t IconGeneration() noexcept
    {
        return g_iconGeneration.load(std::memory_order_acquire);
    }

    uint32_t DisplayLookupContext() noexcept
    {
        if (!g_displayLookupContext ||
            GetTickCount() - g_displayLookupAtMs > 2000u)
        {
            g_displayLookupContext = 0;
            return 0;
        }
        return g_displayLookupContext;
    }

    void ClearDisplayLookupContext() noexcept
    {
        g_displayLookupContext = 0;
        g_displayLookupAtMs = 0;
    }
}

std::string wxl::client::retailitem::VariantIconPath(
    uint32_t itemId, uint32_t modifierId)
{
    if (!itemId || !modifierId) return {};
    const auto catalog = wxl::runtime::db2::retailitems::Current();
    if (!catalog) return {};
    const uint64_t key = (static_cast<uint64_t>(itemId) << 32) | modifierId;
    const auto found = catalog->variants.find(key);
    if (found == catalog->variants.end() || !found->second.iconFileDataId) return {};
    const char* icon = IconForFileDataId(found->second.iconFileDataId);
    if (!icon || !*icon) return {};
    std::string path = "Interface\\Icons\\";
    path += icon;
    return path;
}

bool wxl_modern_m2::InstallRetailItemAccessors()
{
    return ::InstallRetailItemAccessors();
}
