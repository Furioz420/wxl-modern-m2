// Per-slot virtual SKIN resources for retail collection models.
// Copyright (C) 2026 WarcraftXL. GPLv3.

#include "client/CharModel/RetailSkinProvider.hpp"

#include "ExtensionApi.hpp"
#include "engine/assets/shared/textures/blp/BlpTranscode.hpp"
#include "game/Io.hpp"
#include "offsets/engine/Io.hpp"
#include "wxl/ModelDataApi.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
    namespace io = wxl::game::io;
    namespace iooff = wxl::offsets::engine::io;
    namespace blp = wxl::modern::assets::textures::blp;

    std::mutex g_virtualMutex;
    std::unordered_map<std::string, std::vector<uint8_t>> g_virtualFiles;
    std::unordered_map<std::string, std::string> g_preparedPaths;
    std::unordered_map<std::string, std::string> g_componentAliases;
    std::unordered_map<std::string, std::vector<uint8_t>> g_componentFiles;
    thread_local bool g_componentReadThrough = false;

    bool ItemDetailLog()
    {
        static const bool enabled = wxl_modern_m2::ConfigBool(
            "WXL_M2_RETAIL_ITEM_DETAIL_LOG", false);
        return enabled;
    }

    std::string NormalizePath(std::string_view path)
    {
        std::string out(path);
        for (char& c : out)
        {
            if (c == '/') c = '\\';
            else c = static_cast<char>(
                std::tolower(static_cast<unsigned char>(c)));
        }
        if (out.size() >= 4 &&
            out.compare(out.size() - 4, 4, ".mdx") == 0)
            out.replace(out.size() - 4, 4, ".m2");
        return out;
    }

    bool ReadGameFile(const char* path, std::vector<uint8_t>& out) noexcept
    {
        out.clear();
        void* handle = nullptr;
        if (!path || !io::FileOpen(path, iooff::kOpenWholeFile, &handle) ||
            !handle)
            return false;

        uint32_t high = 0;
        const uint32_t size = io::FileSize(handle, &high);
        if (high || !size)
        {
            io::FileClose(handle);
            return false;
        }

        out.resize(size);
        uint32_t read = 0;
        const bool ok = io::FileRead(handle, out.data(), size, &read) != 0 &&
                        read == size;
        io::FileClose(handle);
        if (!ok) out.clear();
        return ok;
    }

    uint16_t ReadU16(const std::vector<uint8_t>& bytes, size_t offset)
    {
        uint16_t value = 0;
        if (offset <= bytes.size() && sizeof(value) <= bytes.size() - offset)
            std::memcpy(&value, bytes.data() + offset, sizeof(value));
        return value;
    }

    uint32_t ReadU32(const std::vector<uint8_t>& bytes, size_t offset)
    {
        uint32_t value = 0;
        if (offset <= bytes.size() && sizeof(value) <= bytes.size() - offset)
            std::memcpy(&value, bytes.data() + offset, sizeof(value));
        return value;
    }

    bool SlotOwnsGroup(uint32_t inventoryType, uint16_t group)
    {
        // Collection M2s commonly contain the geometry for an entire armor
        // set.  Their SKIN section families are the modern collection slot
        // contract. Some collection generations use different families for
        // the same logical slot (for example hands can be 4** or 8**, and
        // legs can be 11** or 13**). Families assigned below are nevertheless
        // exclusive between ordinary equipment slots, so rebuilding one slot
        // cannot leak or erase a neighbouring piece from the shared model.
        switch (inventoryType)
        {
        case 1:  return group == 27;
        case 3:  return group == 26;
        case 4:  return group == 10;
        case 5:  return group == 22 || group == 28;
        case 6:  return group == 18;
        case 7:  return group == 11 || group == 13;
        case 8:  return group == 5 || group == 20;
        case 9:  return group == 8;
        case 10: return group == 4 || group == 23;
        case 16: return group == 15;
        case 19: return group == 9;
        // A robe is the intentional exception: it owns both the torso and
        // robe-skirt collection sections.
        case 20: return group == 22 || group == 13;
        default: return false;
        }
    }

    bool FilterSkin(std::vector<uint8_t>& bytes, uint32_t inventoryType,
                    std::vector<uint16_t>& keptIds) noexcept
    {
        keptIds.clear();
        if (!inventoryType || bytes.size() < 0x2C ||
            std::memcmp(bytes.data(), "SKIN", 4) != 0)
            return false;

        const uint32_t indexCount = ReadU32(bytes, 0x0C);
        const uint32_t indexOffset = ReadU32(bytes, 0x10);
        const uint32_t submeshCount = ReadU32(bytes, 0x1C);
        const uint32_t submeshOffset = ReadU32(bytes, 0x20);
        if (indexOffset > bytes.size() ||
            indexCount > (bytes.size() - indexOffset) / sizeof(uint16_t) ||
            submeshOffset > bytes.size() ||
            submeshCount > (bytes.size() - submeshOffset) / 0x30)
            return false;

        for (uint32_t i = 0; i < submeshCount; ++i)
        {
            const size_t sub = submeshOffset + static_cast<size_t>(i) * 0x30;
            const uint16_t sectionId = ReadU16(bytes, sub);
            if (sectionId &&
                SlotOwnsGroup(inventoryType,
                              static_cast<uint16_t>(sectionId / 100u)) &&
                std::find(keptIds.begin(), keptIds.end(), sectionId) ==
                    keptIds.end())
                keptIds.push_back(sectionId);
        }

        for (uint32_t i = 0; i < submeshCount; ++i)
        {
            const size_t sub = submeshOffset + static_cast<size_t>(i) * 0x30;
            const uint16_t sectionId = ReadU16(bytes, sub);
            if (std::find(keptIds.begin(), keptIds.end(), sectionId) !=
                keptIds.end())
                continue;

            const uint16_t level = ReadU16(bytes, sub + 0x02);
            const uint16_t start16 = ReadU16(bytes, sub + 0x08);
            const uint16_t count16 = ReadU16(bytes, sub + 0x0A);
            uint32_t start = (static_cast<uint32_t>(level) << 16) | start16;
            if (start > indexCount || count16 > indexCount - start)
                start = start16;
            if (start > indexCount || count16 > indexCount - start)
                continue;
            std::memset(bytes.data() + indexOffset +
                            start * sizeof(uint16_t),
                        0, count16 * sizeof(uint16_t));
            // Zeroing the referenced indices hides the geometry, but leaving
            // indexCount intact still makes the native main/shadow renderers
            // submit a degenerate batch for every rejected section. Remove
            // the batch at its authoritative SKIN boundary as well.
            std::memset(bytes.data() + sub + 0x0A, 0, sizeof(uint16_t));
        }
        return true;
    }

    uint32_t HashVariant(std::string_view path,
                         const std::vector<uint16_t>& ids)
    {
        uint32_t hash = 2166136261u;
        const auto mix = [&hash](uint8_t byte) {
            hash ^= byte;
            hash *= 16777619u;
        };
        for (unsigned char c : path) mix(c);
        for (uint16_t id : ids)
        {
            mix(static_cast<uint8_t>(id));
            mix(static_cast<uint8_t>(id >> 8));
        }
        return hash;
    }

    std::string SkinPathFor(std::string_view modelPath)
    {
        std::string skin(modelPath);
        const size_t dot = skin.find_last_of('.');
        if (dot != std::string::npos) skin.resize(dot);
        skin += "00.skin";
        return skin;
    }

    std::string VirtualModelPath(std::string_view realPath, uint32_t hash)
    {
        std::string path(realPath);
        const size_t dot = path.find_last_of('.');
        if (dot != std::string::npos) path.resize(dot);
        char suffix[32]{};
        std::snprintf(suffix, sizeof(suffix), "_wxlgeo_%08x.m2", hash);
        path += suffix;
        return path;
    }

    bool ProvideVirtual(const char* name, std::vector<uint8_t>& out)
    {
        if (!name || g_componentReadThrough) return false;
        const std::string key = NormalizePath(name);
        std::string componentPath;
        {
            std::lock_guard lock(g_virtualMutex);
            if (const auto found = g_virtualFiles.find(key);
                found != g_virtualFiles.end())
            {
                out = found->second;
                return true;
            }
            if (const auto found = g_componentAliases.find(key);
                found != g_componentAliases.end())
                componentPath = found->second;
        }

        if (componentPath.empty()) return false;

        {
            std::lock_guard lock(g_virtualMutex);
            if (const auto cached = g_componentFiles.find(componentPath);
                cached != g_componentFiles.end())
            {
                out = cached->second;
                return true;
            }
        }

        // The registered destination is the real archive path and is never
        // recursively provided. Exact-path aliases are required for retail
        // component rows without a FileDataID suffix, so the nested source
        // FileOpen uses a thread-local read-through guard.
        std::vector<uint8_t> raw;
        struct ReadThrough
        {
            ReadThrough() { g_componentReadThrough = true; }
            ~ReadThrough() { g_componentReadThrough = false; }
        } readThrough;
        if (!ReadGameFile(componentPath.c_str(), raw))
        {
            WLOG_WARN(
                "retail-equipment: component source read failed alias=%s source=%s",
                name, componentPath.c_str());
            return false;
        }

        // This conversion is intentionally scoped to character component
        // aliases. The 3.3.5 compositor consumes paletted BLP2 even though
        // ordinary M2/world texture paths can consume DXT directly.
        static const uint32_t maxEdge = wxl_modern_m2::ConfigU32(
            "WXL_M2_COMPONENT_TEXTURE_MAX_EDGE", 512, 64, 4096);
        bool converted = blp::TextureComponentToPaletted(
            std::span<const uint8_t>(raw.data(), raw.size()), out, maxEdge);
        if (!converted)
        {
            // A smaller set of retail component textures uses encoding 3
            // (uncompressed BGRA). Normalize those to DXT first, then feed the
            // same legacy palettizer. Serving encoding 3 raw makes the 3.3.5
            // character compositor render the component bright green.
            std::vector<uint8_t> dxt;
            converted = blp::TranscodeBlp(
                            std::span<const uint8_t>(raw.data(), raw.size()),
                            dxt) &&
                        blp::TextureComponentToPaletted(
                            std::span<const uint8_t>(dxt.data(), dxt.size()),
                            out, maxEdge);
        }

        if (converted)
        {
            {
                std::lock_guard lock(g_virtualMutex);
                const auto [cached, inserted] =
                    g_componentFiles.try_emplace(componentPath, out);
                if (!inserted) out = cached->second;
            }
            if (ItemDetailLog())
                WLOG_INFO(
                    "retail-equipment: component texture transcoded alias=%s source=%s (%u -> %u bytes)",
                    name, componentPath.c_str(),
                    static_cast<unsigned>(raw.size()),
                    static_cast<unsigned>(out.size()));
            return true;
        }

        out = std::move(raw);
        {
            std::lock_guard lock(g_virtualMutex);
            const auto [cached, inserted] =
                g_componentFiles.try_emplace(componentPath, out);
            if (!inserted) out = cached->second;
        }
        WLOG_WARN(
            "retail-equipment: unsupported component texture alias=%s source=%s bytes=%u",
            name, componentPath.c_str(), static_cast<unsigned>(out.size()));
        return true;
    }

    int __cdecl ProvideVirtualApi(const char* name, const WXL_ByteSink* sink)
    {
        if (!sink || !sink->Write) return 0;
        std::vector<uint8_t> bytes;
        if (!ProvideVirtual(name, bytes)) return 0;
        if (!bytes.empty())
            sink->Write(sink->ctx, bytes.data(), static_cast<unsigned>(bytes.size()));
        return 1;
    }

    bool InstallRetailSkinProviderImpl()
    {
        const WXL_StorageApi* storage = wxl_modern_m2::Storage();
        if (!storage || !storage->RegisterClientProvider)
        {
            WLOG_ERROR("retail-skin: wxl.storage v1 is unavailable");
            return false;
        }
        storage->RegisterClientProvider(&ProvideVirtualApi);
        return true;
    }
}

bool wxl_modern_m2::InstallRetailSkinProvider()
{
    return InstallRetailSkinProviderImpl();
}

bool wxl::client::charmodel::RegisterRetailSkinCompanion(
    std::string_view modelPath, uint32_t skinFileDataId)
{
    if (modelPath.empty() || !skinFileDataId) return false;

    const std::string model = NormalizePath(modelPath);
    const std::string expected = SkinPathFor(model);

    // Most models still ship the legacy name the client derives. Never place
    // a provider in front of a valid archive sibling.
    std::vector<uint8_t> bytes;
    if (ReadGameFile(expected.c_str(), bytes)) return false;

    const auto* modelData = static_cast<const WXL_ModelDataApi*>(
        wxl_modern_m2::g_api->GetInterface("wxl.modeldata",
                                            WXL_MODEL_DATA_API_VERSION));
    if (!modelData || !modelData->FileDataIdForPath) return false;
    const uint32_t modelFileDataId = modelData->FileDataIdForPath(model.c_str());
    if (!modelFileDataId) return false;

    std::string source(model);
    const size_t dot = source.find_last_of('.');
    if (dot != std::string::npos) source.resize(dot);
    source += "_" + std::to_string(modelFileDataId) + "00.skin";
    if (!ReadGameFile(source.c_str(), bytes)) return false;

    {
        std::lock_guard lock(g_virtualMutex);
        g_virtualFiles.try_emplace(expected, std::move(bytes));
    }
    WLOG_INFO("retail-skin: aliased missing companion '%s' -> '%s' (SFID=%u)",
              expected.c_str(), source.c_str(), skinFileDataId);
    return true;
}

void wxl::client::charmodel::RegisterRetailComponentTexturePath(
    std::string_view path, uint32_t componentSection)
{
    if (path.empty() || componentSection >= 8) return;
    std::string key = NormalizePath(path);
    constexpr std::string_view prefix = "item\\texturecomponents\\";
    static constexpr std::array<std::string_view, 8> folders{
        "armuppertexture", "armlowertexture", "handtexture",
        "torsouppertexture", "torsolowertexture", "leguppertexture",
        "leglowertexture", "foottexture",
    };

    // Some retail TextureFilePath rows are intentionally unqualified. The
    // ItemDisplayInfoMaterialRes ComponentSection supplies the compositor
    // folder that is absent from those paths.
    const bool reconstructed = key.rfind(prefix, 0) != 0;
    if (reconstructed)
    {
        const size_t slash = key.find_last_of('\\');
        const std::string_view base = slash == std::string::npos
            ? std::string_view(key)
            : std::string_view(key).substr(slash + 1);
        key = std::string(prefix) + std::string(folders[componentSection]) +
              "\\" + std::string(base);
    }
    // The reconstructed path is not only the virtual alias name: it is also
    // the actual archive path for an unqualified TextureFilePath row. Keeping
    // the original bare filename here makes the read-through miss and leaves
    // the legacy character compositor with a blank/green body layer.
    const std::string resolved = key;

    const size_t dot = key.find_last_of('.');
    if (dot == std::string::npos) return;
    const size_t suffix = dot == std::string::npos
        ? std::string::npos : key.find_last_of('_', dot);
    bool numericSuffix = suffix != std::string::npos && suffix + 1 < dot;
    for (size_t i = numericSuffix ? suffix + 1 : dot; i < dot; ++i)
        if (!std::isdigit(static_cast<unsigned char>(key[i])))
            numericSuffix = false;

    const std::string stem = key.substr(0, dot);
    const std::string extension = key.substr(dot);

    std::lock_guard lock(g_virtualMutex);
    // Exact-path aliases are necessary for components whose retail path ends
    // in the native _M/_F selector rather than a numeric FileDataID. The
    // provider's scoped read-through guard keeps their source read recursive-safe.
    g_componentAliases.try_emplace(key, resolved);
    // Legacy rows omit the FileDataID suffix. Exact dynamic rows retain it,
    // after which the 3.3.5 compositor may append a body selector.
    if (numericSuffix)
    {
        std::string legacy = key;
        legacy.erase(suffix, dot - suffix);
        g_componentAliases.try_emplace(std::move(legacy), resolved);
    }
    for (char selector : {'m', 'f', 'u'})
        g_componentAliases.try_emplace(
            stem + '_' + selector + extension, resolved);
}

bool wxl::client::charmodel::PrepareRetailSkinPath(
    const char* realPath, uint32_t inventoryType,
    char* outPath, size_t outSize) noexcept
{
    if (!realPath || !*realPath || !outPath || !outSize || !inventoryType)
        return false;

    try
    {
        const std::string normalized = NormalizePath(realPath);
        const std::string preparedKey =
            normalized + "|" + std::to_string(inventoryType);
        {
            std::lock_guard lock(g_virtualMutex);
            if (const auto cached = g_preparedPaths.find(preparedKey);
                cached != g_preparedPaths.end())
            {
                if (cached->second.size() + 1 > outSize) return false;
                std::memcpy(outPath, cached->second.c_str(),
                            cached->second.size() + 1);
                return true;
            }
        }

        std::vector<uint8_t> skin;
        if (!ReadGameFile(SkinPathFor(normalized).c_str(), skin))
            return false;

        std::vector<uint16_t> ids;
        if (!FilterSkin(skin, inventoryType, ids)) return false;
        // Inventory types that retain the same sections from the same source
        // model must resolve to one signature. The equipment owner uses this
        // stable virtual path to prevent duplicate robe/legs collection
        // geometry from being attached twice.
        const uint32_t hash = HashVariant(normalized, ids);
        const std::string virtualModel = VirtualModelPath(normalized, hash);
        const std::string virtualSkin = SkinPathFor(virtualModel);
        if (virtualModel.size() + 1 > outSize) return false;

        bool needsModel = false;
        {
            std::lock_guard lock(g_virtualMutex);
            if (g_virtualFiles.contains(virtualModel) &&
                g_virtualFiles.contains(virtualSkin))
            {
                g_preparedPaths.emplace(preparedKey, virtualModel);
                std::memcpy(outPath, virtualModel.c_str(),
                            virtualModel.size() + 1);
                return true;
            }
            needsModel = !g_virtualFiles.contains(virtualModel);
        }

        std::vector<uint8_t> model;
        if (needsModel && !ReadGameFile(normalized.c_str(), model))
            return false;

        {
            std::lock_guard lock(g_virtualMutex);
            const bool fresh =
                !g_virtualFiles.contains(virtualModel) ||
                !g_virtualFiles.contains(virtualSkin);
            if (!g_virtualFiles.contains(virtualModel))
                g_virtualFiles.emplace(virtualModel, std::move(model));
            if (!g_virtualFiles.contains(virtualSkin))
                g_virtualFiles.emplace(virtualSkin, std::move(skin));
            g_preparedPaths.emplace(preparedKey, virtualModel);
            if (fresh && ItemDetailLog())
                WLOG_INFO(
                    "retail-skin: prepared inventory=%u sections=%u model=%s",
                    inventoryType, static_cast<unsigned>(ids.size()),
                    virtualModel.c_str());
        }
        std::memcpy(outPath, virtualModel.c_str(), virtualModel.size() + 1);
        return true;
    }
    catch (...)
    {
        return false;
    }
}

namespace wxl::client::charmodel
{
    RetailSkinMemoryStats GetRetailSkinMemoryStats()
    {
        RetailSkinMemoryStats stats;
        std::lock_guard lock(g_virtualMutex);
        stats.virtualFiles = g_virtualFiles.size();
        stats.componentFiles = g_componentFiles.size();
        stats.preparedPaths = g_preparedPaths.size();
        stats.componentAliases = g_componentAliases.size();
        for (const auto& entry : g_virtualFiles)
            stats.virtualBytes += entry.second.capacity();
        for (const auto& entry : g_componentFiles)
            stats.componentBytes += entry.second.capacity();
        return stats;
    }
}
