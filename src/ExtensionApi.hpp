// wxl-modern-m2: the extension-wide service table pointer and hook-install convenience, shared by every
// translation unit in this DLL.
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

#pragma once

#include "common/ExtensionConfig.hpp"
#include "wxl/FdidApi.h"
#include "wxl/FrameScriptApi.h"
#include "wxl/ItemVariantApi.h"
#include "wxl/M2ArenaApi.h"
#include "wxl/M2AnimationApi.h"
#include "wxl/M2DrawApi.h"
#include "wxl/NetworkApi.h"
#include "wxl/PluginApi.h"
#include "wxl/RetailDb2Api.h"
#include "wxl/RetailSpellDb2Api.h"
#include "wxl/StorageApi.h"

#include "engine/events/Event.hpp"

#include <windows.h>

#include <cstdint>
#include <cstdlib>

/// See wxl-adt's ExtensionApi.hpp for the reasoning behind every pattern here: the core hands this
/// pointer to WXL_Load once and it lives for the process lifetime, so every detour installed later
/// reaches it through here rather than threading an `api` parameter through the call chain.
namespace wxl_modern_m2
{
    extern const WXL_Api* g_api;

    /// True builds the whole native M2 pipeline in; false is the single point that would leave the
    /// client on its stock M2 reader.
    inline constexpr bool kEnabled = true;

    // --- wxl-db2's FileDataID resolver, fetched lazily (extensions load alphabetically; wxl-db2
    // loads before wxl-modern-m2, so this always resolves once anything actually asks for a path). ----------
    extern const WXL_FdidApi* g_fdid;
    extern const WXL_RetailDb2Api* g_retailDb2;
    extern const WXL_RetailSpellDb2Api* g_retailSpellDb2;
    extern const WXL_FrameScriptApi* g_frameScript;
    extern const WXL_NetworkApi* g_network;
    extern const WXL_StorageApi* g_storage;

    inline const WXL_FdidApi* Fdid()
    {
        if (!g_fdid)
        {
            // API v2 only appends ResolveMaterialTexture; the texture/model resolver prefix used by
            // this module is unchanged from v1. Prefer the current contract, but remain loadable
            // beside a v1 wxl-db2 while Hub installations transition the modules independently.
            void* iface = g_api->GetInterface("wxl.fdid", WXL_FDID_API_VERSION);
            if (!iface && WXL_FDID_API_VERSION > 1)
                iface = g_api->GetInterface("wxl.fdid", 1);
            g_fdid = static_cast<const WXL_FdidApi*>(iface);
        }
        return g_fdid;
    }

    inline const char* ResolveTexture(uint32_t fileDataId)
    {
        const WXL_FdidApi* fdid = Fdid();
        return fdid ? fdid->ResolveTexture(fileDataId) : nullptr;
    }

    inline const char* ResolveModel(uint32_t fileDataId)
    {
        const WXL_FdidApi* fdid = Fdid();
        return fdid ? fdid->ResolveModel(fileDataId) : nullptr;
    }

    inline const WXL_RetailDb2Api* RetailDb2()
    {
        if (!g_retailDb2)
            g_retailDb2 = static_cast<const WXL_RetailDb2Api*>(
                g_api->GetInterface("wxl.retail-db2", WXL_RETAIL_DB2_API_VERSION));
        return g_retailDb2;
    }

    inline const WXL_RetailSpellDb2Api* RetailSpellDb2()
    {
        if (!g_retailSpellDb2)
            g_retailSpellDb2 = static_cast<const WXL_RetailSpellDb2Api*>(
                g_api->GetInterface("wxl.retail-spell-db2", WXL_RETAIL_SPELL_DB2_API_VERSION));
        return g_retailSpellDb2;
    }

    inline const WXL_FrameScriptApi* FrameScript()
    {
        if (!g_frameScript)
            g_frameScript = static_cast<const WXL_FrameScriptApi*>(
                g_api->GetInterface("wxl.framescript", WXL_FRAME_SCRIPT_API_VERSION));
        return g_frameScript;
    }

    inline const WXL_NetworkApi* Network()
    {
        if (!g_network)
            g_network = static_cast<const WXL_NetworkApi*>(
                g_api->GetInterface("wxl.network", WXL_NETWORK_API_VERSION));
        return g_network;
    }

    inline const WXL_StorageApi* Storage()
    {
        if (!g_storage)
            g_storage = static_cast<const WXL_StorageApi*>(
                g_api->GetInterface("wxl.storage", WXL_STORAGE_API_VERSION));
        return g_storage;
    }

    // --- the core-owned large-M2 arena, published (Boot phase, before any extension loads) as
    // "wxl.m2arena" -- always present by the time wxl-modern-m2's own WXL_Load runs, but fetched through the
    // same lazy accessor as every other cross-binary service for one uniform pattern. -----------------
    extern const WXL_M2ArenaApi* g_arena;

    inline const WXL_M2ArenaApi* Arena()
    {
        if (!g_arena)
            g_arena = static_cast<const WXL_M2ArenaApi*>(g_api->GetInterface("wxl.m2arena", WXL_M2ARENA_API_VERSION));
        return g_arena;
    }

    /**
     * @brief Typed detour install over WXL_Api::HookAttach: detour and original share one function
     *        type, deduced, so wiring a hook to the wrong original no longer compiles. Mirrors
     *        wxl::hook::Install's own convenience overload, which this extension cannot link against.
     */
    template <class Fn>
    inline bool HookAttach(const char* name, uintptr_t target, Fn* detour, Fn** original,
                           int priority = WXL_HOOK_DEFAULT_PRIORITY)
    {
        return g_api->HookAttach(name, target, reinterpret_cast<void*>(detour),
                                 reinterpret_cast<void**>(original), priority) != 0;
    }

    template <class Args>
    inline void Emit(wxl::events::Event event, const Args* args)
    {
        g_api->Emit(static_cast<uint32_t>(event), args);
    }

    /**
     * @brief Typed detour install over WXL_Api::HookAttachByName: resolves `pointName` ("Namespace.Name")
     *        against the core's centralized HookPoints table instead of carrying a raw offset here.
     */
    template <class Fn>
    inline bool HookAttachByName(const char* pointName, Fn* detour, Fn** original,
                                 int priority = WXL_HOOK_DEFAULT_PRIORITY)
    {
        return g_api->HookAttachByName(pointName, reinterpret_cast<void*>(detour),
                                       reinterpret_cast<void**>(original), priority) != 0;
    }

    // Per-file installers, called from WXL_Load in Module.cpp.
    bool InstallM2CompatBones();       // BonePalette.cpp (gated)
    bool InstallEmitterBlend();        // EmitterBlend.cpp (gated)
    bool InstallM2PerFrameUpdate();    // PerFrameUpdate.cpp (gated)
    bool InstallM2SceneHitTestSort();  // HitTestSort.cpp (gated)
    bool InstallM2Draw();              // M2Draw.cpp (unconditional -- owns the DIP vtable slot)
    bool InstallM2SetupBatchAlpha();   // SetupMaterial.cpp (gated)
    bool InstallCombinerPatch();       // render/CombinerPatch.cpp (gated)
    bool InstallAnimUnwrap();          // AnimUnwrap.cpp (gated)
    bool InstallExtendedAnimations();  // client/Animation/AnimationResolver.cpp (native service)
    bool InstallM2CompatLoader();      // CompatLoader.cpp (gated)
    bool InstallM2Native();            // NativeLoad.cpp (gated)
    bool InstallHdSwitch();            // load/HdSwitch.cpp (retail character family redirect)
    bool InstallM2Memory();            // Memory.cpp (gated)
    bool InstallModernM2();            // ModernM2.cpp (gated)
    bool InstallCharacterGeosets();    // compat/CharacterGeosets.cpp
    bool InstallCharacterSheet();      // compat/CharacterSheet.cpp
    bool InstallCharacterTextures();   // compat/CharacterTextures.cpp
    bool InstallCharacterCustomize();  // compat/CharacterCustomize.cpp
    bool InstallModelIndices();        // render/ModelIndices.cpp

    struct SheetRect { uint32_t x, y, width, height; };

    enum class LayerPaint
    {
        Painted,
        Idle,
        Reading,
    };

    // Private compatibility mode: InferAlpha with a soft lower edge. Retail Pandaren male layout
    // 129 omits the torso target row and its synthesized replacement otherwise ends at a hard cell.
    constexpr uint32_t kLayerBlendFeatherBottom = 0xFFFFFFFEu;

    LayerPaint PaintLayer(uint32_t source, void* const* destLevels, uint32_t pitch,
                          uint32_t blend, const SheetRect& span, const SheetRect& dest);

    bool RaceOfModelPath(const char* path, uint32_t& chrRaceId, uint32_t& sex);
    uint32_t RetailCharacterRace(uint32_t chrRaceId);
    bool RetailCharacterCanaryAllows(uint32_t chrRaceId, uint32_t sex);
    bool IsModernCharacterComposition();
    void SetCharacterSheetGearVisible(bool visible);
    void CustomizeNoteModel(uint32_t chrModel, uint32_t chrRaceId, uint32_t sex);
    void CustomizeNoteSelection();
    void CustomizeNoteComponent(void* component);
    void* CustomizeComponent();
    bool CustomizeIdentity(uint32_t& chrRaceId, uint32_t& sex);
    uint32_t CustomizeClass();
    uint32_t CustomizeGeneration();
    uint32_t LegacyBrokenHairColor();
    uint32_t LegacyBrokenHornStyle();
    uint32_t MurlocFemaleSkin();
    uint32_t MurlocFemaleOutfit();
    int CustomizeChoiceFor(uint32_t chrModel, uint32_t optionIndex);
    bool InstallServerAppearance();
    void NotifyAppearanceVisualsChanged(void* cmo);
    bool RefreshAppearanceGeometry(void* cmo);
    void RefreshEquipmentGeometry(void* cmo);
    bool CustomizationAttachmentsReady(void* cmo) noexcept;
    void ForgetCharacterEquipment(void* cmo) noexcept;
    bool ServerAlternateAppearanceIdentity(const void* cmo, uint32_t& retailRace, uint32_t& sex);
    bool ServerAppearanceChoices(void* cmo, uint32_t model, uint32_t* out, uint32_t capacity, uint32_t& count);
    int ServerPrivateAppearanceChoice(void* cmo, uint32_t race, uint32_t sex, uint32_t option);
    int ServerAppearanceChoice(void* cmo, uint32_t model, uint32_t option);
    uint32_t SettledChoicesFor(void* cmo, uint32_t chrModel, uint32_t* out, uint32_t capacity);
    void* CustomizationWholeTexture(void* rootInstance, uint32_t textureType);

    struct CustomizationAttachmentSpec
    {
        uint32_t fileDataId;
        uint32_t geoset;
    };

    /// Publishes the selected root-skinned customization children to the scene owner. The consumer
    /// copies the array before returning; callers retain no storage or lifetime obligation.
    void QueueCustomizationAttachments(
        void* cmo, void* rootInstance, uint32_t chrModel, uint32_t generation,
        const CustomizationAttachmentSpec* attachments, uint32_t attachmentCount);

    /// Immediately detaches and forgets root-skinned customization children. Glue may recycle the
    /// same CMO/root address across race or sex changes, so pointer identity alone cannot retire the
    /// previous appearance recipe.
    void ResetCustomizationAttachments() noexcept;
    void ReleaseCustomizationAttachments(void* cmo) noexcept;

    constexpr uint32_t kNoIsolation = 0xFFFFFFFFu;
    constexpr uint32_t kHideEverything = 0xFFFEu;
    uint32_t ShownGeosets(uint16_t* out, uint32_t capacity);
    uint32_t IsolatedGeoset();
    void SetIsolatedGeoset(uint32_t id);
    const char* GeosetGroupName(uint32_t geosetId);

    uint32_t BisectTriangleCount();
    bool BisectArmed();
    void SetBisectArmed(bool armed);
    void BisectRange(uint32_t& first, uint32_t& count);
    void SetBisectRange(uint32_t first, uint32_t count);
    void RequestBisectReport();

    constexpr uint32_t kEverySection = 0xFFFFFFFFu;
    uint32_t PaintModernLayers(void* cmo, uint32_t chrRaceId, uint32_t sex, uint32_t layoutId,
                               void* const* destLevels, uint32_t width, uint32_t height,
                               uint32_t stockRight, uint32_t sectionFilter,
                               uint32_t& outReading);
    bool InstallNativeItemDbc();       // client/Item/NativeItemDbc.cpp (optional)
    bool InstallItemVariantBridge();   // client/FrameScript/WxlWow.cpp (optional)
    bool InstallRetailItemAccessors(); // client/Item/RetailItem.cpp (optional)
    bool InstallCharModel();           // client/CharModel/CharModel.cpp (optional)
    void SetCollectionPreviewReset(bool active) noexcept;
    bool InstallRetailEquipment();     // client/CharModel/RetailEquipment.cpp (optional)
    void PrepareNativeEquipmentSlot(void* cmo, uint32_t modelSlot);
    void* SetEquipmentModelOwner(void* cmo) noexcept;
    struct EquipmentModelScope {
        void* previous;
        explicit EquipmentModelScope(void* cmo) : previous(SetEquipmentModelOwner(cmo)) {}
        ~EquipmentModelScope() { SetEquipmentModelOwner(previous); }
        EquipmentModelScope(const EquipmentModelScope&)=delete;
        EquipmentModelScope& operator=(const EquipmentModelScope&)=delete;
    };
    bool SuppressGlueRetailDisplay(void* cmo, uint32_t displayId);
    bool InstallRetailSkinProvider();  // client/CharModel/RetailSkinProvider.cpp (optional)
    bool InstallRetailSpellVisuals();  // client/Spell/RetailSpellVisual.cpp (optional)
    bool InstallLegacySpellPresentation(); // default-off native kit projection; no gameplay changes
    bool InstallM2LodVariant();        // load/LodModelVariant.cpp (unconditional -- inert until a
                                        // caller actually requests a synthetic per-tier name)

    inline bool ConfigTruthy(const char* raw, bool fallback) { return wxl::ext::config::Truthy(raw, fallback); }

    inline bool ConfigRaw(const char* name, char* buf, size_t cap)
    {
        return wxl::ext::config::Raw(name, buf, cap, "Extensions\\wxl-modern-m2\\wxl-modern-m2.cfg");
    }

    inline bool ConfigBool(const char* name, bool fallback)
    {
        char value[16] = {};
        return ConfigRaw(name, value, sizeof value)
            ? ConfigTruthy(value, fallback)
            : fallback;
    }

    /// Feature toggle: an env var (falsy value disables) plus a .disable sentinel FILE (checked
    /// directly, independent of the .cfg mechanism), default ON.
    inline bool ConfigFlag(const char* envName, const char* disableFile)
    {
        char value[16] = {};
        if (ConfigRaw(envName, value, sizeof value) && !ConfigTruthy(value, true))
            return false;
        if (disableFile && GetFileAttributesA(disableFile) != INVALID_FILE_ATTRIBUTES)
            return false;
        return true;
    }

    inline uint64_t ConfigU64(const char* name, uint64_t fallback, uint64_t minValue, uint64_t maxValue)
    {
        char value[32] = {};
        if (!ConfigRaw(name, value, sizeof value)) return fallback;
        char* end = nullptr;
        const uint64_t parsed = std::strtoull(value, &end, 10);
        if (end == value) return fallback;
        if (parsed < minValue) return minValue;
        if (parsed > maxValue) return maxValue;
        return parsed;
    }

    inline uint32_t ConfigU32(const char* name, uint32_t fallback, uint32_t minValue, uint32_t maxValue)
    {
        return static_cast<uint32_t>(ConfigU64(name, fallback, minValue, maxValue));
    }

    /// Resolves a byte-count setting: an MB env var, then a KB one, then a default; a candidate
    /// outside [minKb, maxKb] is rejected and the next source tried.
    inline uint32_t ConfigBytesMbKb(const char* envMb, const char* envKb, uint32_t defBytes,
                                    uint32_t minKb, uint32_t maxKb)
    {
        char value[32] = {};
        if (ConfigRaw(envMb, value, sizeof value))
        {
            char* end = nullptr;
            const uint64_t mb = std::strtoull(value, &end, 10);
            const uint64_t kb = mb * 1024ull;
            if (end != value && kb >= minKb && kb <= maxKb)
                return static_cast<uint32_t>(kb * 1024ull);
        }
        if (ConfigRaw(envKb, value, sizeof value))
        {
            char* end = nullptr;
            const uint64_t kb = std::strtoull(value, &end, 10);
            if (end != value && kb >= minKb && kb <= maxKb)
                return static_cast<uint32_t>(kb * 1024ull);
        }
        return defBytes;
    }
}

// common/Log.hpp's WLOG_* macros need common/Log.cpp linked in, which is core/host/patcher-only (see
// its own doc comment) -- an extension has no such object file, so these route the same call-site
// syntax through WXL_Api::Log instead.
#define WLOG_TRACE(...) ::wxl_modern_m2::g_api->Log(WXL_LOG_TRACE, "wxl-modern-m2", __VA_ARGS__)
#define WLOG_DEBUG(...) ::wxl_modern_m2::g_api->Log(WXL_LOG_DEBUG, "wxl-modern-m2", __VA_ARGS__)
#define WLOG_INFO(...)  ::wxl_modern_m2::g_api->Log(WXL_LOG_INFO,  "wxl-modern-m2", __VA_ARGS__)
#define WLOG_WARN(...)  ::wxl_modern_m2::g_api->Log(WXL_LOG_WARN,  "wxl-modern-m2", __VA_ARGS__)
#define WLOG_ERROR(...) ::wxl_modern_m2::g_api->Log(WXL_LOG_ERROR, "wxl-modern-m2", __VA_ARGS__)
