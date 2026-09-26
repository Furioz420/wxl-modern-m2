// Supplemental indexes for complete WotLK-shaped item DBC catalogs.
// Copyright (C) 2026 WarcraftXL. GPLv3.

#include "client/Item/NativeItemDbc.hpp"
#include "client/PrivateClientOffsets.hpp"
#include "client/CharModel/RetailSkinProvider.hpp"
#include "ExtensionApi.hpp"
#include "game/Io.hpp"
#include "offsets/game/DB2.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace wxl::client::nativeitemdbc
{
    namespace
    {
        constexpr size_t kItemFields = 8;
        using ItemRow = std::array<uint32_t, kItemFields>;
        std::unordered_map<uint32_t, ItemRow> g_itemRows;
        std::unordered_set<uint32_t> g_displayIds;
        std::unordered_map<uint32_t, std::array<uint32_t, 2>> g_helmetVisibility;
        std::unordered_map<uint32_t, std::string> g_displayIcons;
    }

    bool SupplementalItemDisplay(
        uint32_t itemId, uint32_t& displayId) noexcept
    {
        displayId = 0;
        const auto found = g_itemRows.find(itemId);
        if (found == g_itemRows.end()) return false;
        displayId = found->second[5];
        return true;
    }

    bool SupplementalItemField(
        uint32_t itemId, size_t offset, uint32_t& value) noexcept
    {
        value = 0;
        if ((offset % sizeof(uint32_t)) != 0 ||
            offset >= kItemFields * sizeof(uint32_t))
            return false;
        const auto found = g_itemRows.find(itemId);
        if (found == g_itemRows.end()) return false;
        value = found->second[offset / sizeof(uint32_t)];
        return true;
    }

    bool SupplementalHelmetVisibility(uint32_t displayId, uint32_t sex, uint32_t& visibility) noexcept
    {
        const auto found = g_helmetVisibility.find(displayId);
        if (found == g_helmetVisibility.end() || sex > 1) return false;
        visibility = found->second[sex];
        return true;
    }

    bool SupplementalDisplayExists(uint32_t displayId) noexcept
    {
        return g_displayIds.contains(displayId);
    }

    const char* SupplementalDisplayIcon(uint32_t displayId) noexcept
    {
        const auto found = g_displayIcons.find(displayId);
        return found == g_displayIcons.end()
            ? nullptr : found->second.c_str();
    }
}

namespace
{
    namespace db2off = wxl::offsets::game::db2;
    namespace nativeitemdbc = wxl::client::nativeitemdbc;

    constexpr uint32_t kWdbcMagic = 0x43424457u; // "WDBC"
    constexpr uint32_t kMaxRows = 1000000u;
    constexpr uint32_t kMaxStringBytes = 64u * 1024u * 1024u;
    constexpr char kItemPath[] = "DBFilesClient\\Item.dbc";
    constexpr char kItemDisplayPath[] =
        "DBFilesClient\\ItemDisplayInfo.dbc";
    constexpr uint32_t kItemDisplayFields = 25;
    constexpr uint32_t kItemDisplayIcon1Field = 5;
    constexpr uint32_t kItemDisplayIcon2Field = 6;
    constexpr uint32_t kItemDisplayComponentFirstField = 15;
    constexpr uint32_t kItemDisplayComponentCount = 8;
    constexpr uint32_t kImportedDisplayFloor = 100000;
    static_assert(kItemDisplayFields * sizeof(uint32_t) ==
                  wxl_modern_m2::private_offsets::db2::itemdisplayinfo::kCompactRecordSize,
                  "Wrath ItemDisplayInfo rows must remain 25 DWORDs");

#pragma pack(push, 1)
    struct DbcHeader
    {
        uint32_t magic;
        uint32_t records;
        uint32_t fields;
        uint32_t recordSize;
        uint32_t stringBytes;
    };
#pragma pack(pop)
    static_assert(sizeof(DbcHeader) == 20);
    static_assert(sizeof(void*) == sizeof(uint32_t));

    bool ReadArchiveFile(const char* path, std::vector<uint8_t>& bytes)
    {
        bytes.clear();
        void* handle = nullptr;
        if (!wxl::game::io::FileOpen(
                path, wxl::game::io::kOpenWholeFile, &handle) ||
            !handle)
            return false;

        struct FileLease
        {
            void* handle;
            ~FileLease()
            {
                if (handle) wxl::game::io::FileClose(handle);
            }
        } lease{handle};

        uint32_t high = 0;
        const uint32_t size = wxl::game::io::FileSize(handle, &high);
        if (!size || high) return false;

        bytes.resize(size);
        uint32_t read = 0;
        if (!wxl::game::io::FileRead(
                handle, bytes.data(), size, &read) ||
            read != size)
        {
            bytes.clear();
            return false;
        }
        return true;
    }

    bool HasTerminator(const char* value, size_t remaining)
    {
        return std::memchr(value, '\0', remaining) != nullptr;
    }

    bool EndsWithUniversalSelector(std::string_view value)
    {
        return value.size() >= 2 &&
               value[value.size() - 2] == '_' &&
               (value.back() == 'u' || value.back() == 'U');
    }

    bool IndexItemDisplays(std::span<const uint8_t> bytes)
    {
        if (bytes.size() < sizeof(DbcHeader)) return false;

        DbcHeader header{};
        std::memcpy(&header, bytes.data(), sizeof(header));
        if (header.magic != kWdbcMagic ||
            header.fields != 8 ||
            header.recordSize != db2off::item::kRecordSize ||
            !header.records || header.records > kMaxRows ||
            !header.stringBytes ||
            header.stringBytes > kMaxStringBytes)
            return false;

        const uint64_t recordBytes =
            static_cast<uint64_t>(header.records) * header.recordSize;
        const uint64_t required =
            sizeof(DbcHeader) + recordBytes + header.stringBytes;
        if (required != bytes.size()) return false;

        const uint8_t* records = bytes.data() + sizeof(DbcHeader);
        std::unordered_map<uint32_t, nativeitemdbc::ItemRow> indexed;
        indexed.reserve(header.records);
        for (uint32_t rowIndex = 0;
             rowIndex < header.records; ++rowIndex)
        {
            const auto* row = reinterpret_cast<const uint32_t*>(
                records +
                static_cast<size_t>(rowIndex) * header.recordSize);
            nativeitemdbc::ItemRow copy{};
            std::copy_n(row, copy.size(), copy.begin());
            if (!row[0] || !indexed.emplace(row[0], copy).second)
                return false;
        }

        wxl::client::nativeitemdbc::g_itemRows =
            std::move(indexed);
        WLOG_INFO(
            "native-item-dbc: indexed early Item.dbc rows=%u",
            header.records);
        return true;
    }

    bool IndexDisplayIcons(std::span<const uint8_t> bytes)
    {
        if (bytes.size() < sizeof(DbcHeader)) return false;

        DbcHeader header{};
        std::memcpy(&header, bytes.data(), sizeof(header));
        if (header.magic != kWdbcMagic ||
            header.fields != kItemDisplayFields ||
            header.recordSize !=
                wxl_modern_m2::private_offsets::db2::itemdisplayinfo::kCompactRecordSize ||
            !header.records || header.records > kMaxRows ||
            !header.stringBytes ||
            header.stringBytes > kMaxStringBytes)
            return false;

        const uint64_t recordBytes =
            static_cast<uint64_t>(header.records) * header.recordSize;
        const uint64_t required =
            sizeof(DbcHeader) + recordBytes + header.stringBytes;
        if (required != bytes.size()) return false;

        const uint8_t* records = bytes.data() + sizeof(DbcHeader);
        const char* strings = reinterpret_cast<const char*>(
            records + recordBytes);
        if (strings[0] != '\0') return false;

        std::unordered_map<uint32_t, std::string> indexed;
        indexed.reserve(header.records);
        std::unordered_map<uint32_t, std::array<uint32_t, 2>> helmetVisibility;
        std::unordered_set<uint32_t> displayIds;
        displayIds.reserve(header.records);
        uint32_t minId = (std::numeric_limits<uint32_t>::max)();
        uint32_t maxId = 0;
        uint32_t importedComponentAliases = 0;

        for (uint32_t rowIndex = 0;
             rowIndex < header.records; ++rowIndex)
        {
            const auto* row = reinterpret_cast<const uint32_t*>(
                records +
                static_cast<size_t>(rowIndex) * header.recordSize);
            const uint32_t id = row[0];
            if (!id || !displayIds.insert(id).second) return false;
            helmetVisibility.emplace(id, std::array<uint32_t, 2>{row[13], row[14]});

            minId = (std::min)(minId, id);
            maxId = (std::max)(maxId, id);

            const char* icon = nullptr;
            for (uint32_t field :
                 {kItemDisplayIcon1Field, kItemDisplayIcon2Field})
            {
                const uint32_t offset = row[field];
                if (offset >= header.stringBytes ||
                    !HasTerminator(
                        strings + offset,
                        header.stringBytes - offset))
                    return false;
                if (!icon && strings[offset] != '\0')
                    icon = strings + offset;
            }

            if (icon && !indexed.emplace(id, icon).second)
                return false;

            // Imported post-Wrath component rows commonly point at a single
            // universal source such as *_u.blp. The 3.3.5 character
            // compositor appends its own _M/_F selector to the DBC string,
            // otherwise requesting the nonexistent *_u_F.blp and leaving the
            // body layer flesh-coloured. Reuse the retail component provider
            // to map those derived requests back to the mounted *_u.blp.
            if (id >= kImportedDisplayFloor)
            {
                for (uint32_t section = 0;
                     section < kItemDisplayComponentCount; ++section)
                {
                    const uint32_t offset =
                        row[kItemDisplayComponentFirstField + section];
                    if (!offset) continue;
                    if (offset >= header.stringBytes ||
                        !HasTerminator(strings + offset,
                                       header.stringBytes - offset))
                        return false;

                    const std::string_view texture(strings + offset);
                    if (!EndsWithUniversalSelector(texture)) continue;
                    std::string source(texture);
                    source += ".blp";
                    wxl::client::charmodel::
                        RegisterRetailComponentTexturePath(source, section);
                    ++importedComponentAliases;
                }
            }
        }

        wxl::client::nativeitemdbc::g_helmetVisibility = std::move(helmetVisibility);
        wxl::client::nativeitemdbc::g_displayIcons =
            std::move(indexed);
        wxl::client::nativeitemdbc::g_displayIds =
            std::move(displayIds);
        WLOG_INFO(
            "native-item-dbc: indexed ItemDisplayInfo.dbc "
            "rows=%u icons=%zu ids=%u..%u componentAliases=%u",
            header.records,
            wxl::client::nativeitemdbc::g_displayIcons.size(),
            minId, maxId, importedComponentAliases);
        return true;
    }

    bool IndexNativeItemCatalogs()
    {
        std::vector<uint8_t> itemBytes;
        if (!ReadArchiveFile(kItemPath, itemBytes))
        {
            WLOG_WARN(
                "native-item-dbc: host Item.dbc unavailable; "
                "keeping startup table");
            return true;
        }

        if (!IndexItemDisplays(itemBytes))
        {
            WLOG_ERROR(
                "native-item-dbc: failed to index host Item.dbc");
            return true;
        }

        std::vector<uint8_t> displayBytes;
        if (!ReadArchiveFile(kItemDisplayPath, displayBytes))
        {
            WLOG_WARN(
                "native-item-dbc: host ItemDisplayInfo.dbc unavailable; "
                "missing startup display icons may fall through to DB2");
        }
        else if (!IndexDisplayIcons(displayBytes))
        {
            WLOG_ERROR(
                "native-item-dbc: rejected malformed host "
                "ItemDisplayInfo.dbc icon index");
        }

        // v1.1 keeps WoW's startup-loaded native DBC storage authoritative.
        // Replacing its record and id-table pointers after startup invalidates
        // references held by native equipment and character-model consumers.
        // These immutable supplemental indexes serve only the retail bridge.
        WLOG_INFO(
            "native-item-dbc: supplemental catalogs indexed; "
            "preserved client-owned Item.dbc storage");
        return true;
    }
}

bool wxl_modern_m2::InstallNativeItemDbc()
{
    return IndexNativeItemCatalogs();
}
