// wxl-modern-m2: native M2 (MD21/M3) reader + M2 render/compat pipeline, as an out-of-core extension.
// Entry point.
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

#include "ExtensionApi.hpp"
#include "compat/GilneanPreview.hpp"
#include "wxl/EventScript.hpp"

const WXL_PluginInfo* __cdecl WXL_Query(void)
{
    static const WXL_PluginInfo info = {
        sizeof(WXL_PluginInfo),
        WXL_API_VERSION,
        "wxl-modern-m2",
        1,
        WXL_CLIENT_BUILD,
    };
    return &info;
}

int __cdecl WXL_Load(const WXL_Api* api)
{
    if (!api || api->apiVersion != WXL_API_VERSION) return 0;

    wxl_modern_m2::g_api = api;
    wxl::ext::EventScript::Bind(api);

    // M2Draw is unconditional: the 32-bit start-index expansion and ribbon multi-texture fold are
    // stock-compatibility fixes, not modern-only features (ex core render's InstallM2DrawHooks(),
    // always called regardless of modernM2Support). It also owns the DrawIndexedPrimitive vtable slot
    // and publishes wxl.m2draw, so it must install before anything that might need that interface.
    wxl_modern_m2::InstallM2Draw();

    if constexpr (wxl_modern_m2::kEnabled)
    {
        wxl_modern_m2::InstallM2Memory();
        wxl_modern_m2::InstallM2CompatBones();
        wxl_modern_m2::InstallEmitterBlend();
        wxl_modern_m2::InstallM2PerFrameUpdate();
        wxl_modern_m2::InstallM2SceneHitTestSort();
        wxl_modern_m2::InstallM2SetupBatchAlpha();
        wxl_modern_m2::InstallCombinerPatch();
        wxl_modern_m2::InstallAnimUnwrap();
        wxl_modern_m2::InstallM2CompatLoader();
        wxl_modern_m2::InstallM2Native();
        wxl_modern_m2::InstallModernM2();

        if (wxl_modern_m2::ConfigBool("WXL_M2_RETAIL_CHARACTERS", false))
        {
            wxl_modern_m2::InstallHdSwitch();
            wxl_modern_m2::InstallCharacterGeosets();
            wxl_modern_m2::InstallCharacterSheet();
            wxl_modern_m2::InstallGilneanPreview();
            wxl_modern_m2::InstallCharacterCustomize();
            wxl_modern_m2::InstallServerAppearance();
            wxl_modern_m2::InstallCharacterTextures();
            wxl_modern_m2::InstallModelIndices();
            api->Log(WXL_LOG_INFO, "wxl-modern-m2",
                     "native Retail character-model pipeline enabled");
        }
        else
            api->Log(WXL_LOG_INFO, "wxl-modern-m2",
                     "native Retail character-model pipeline disabled by configuration");

        if (!wxl_modern_m2::InstallExtendedAnimations())
            api->Log(WXL_LOG_WARN, "wxl-modern-m2",
                     "extended animation resolver unavailable");

        if (wxl_modern_m2::ConfigBool("WXL_M2_RETAIL_ITEMS", true))
        {
            // Provider and DBC catalogs precede the accessors/controllers that consume them.
            wxl_modern_m2::InstallRetailSkinProvider();
            wxl_modern_m2::InstallNativeItemDbc();
            wxl_modern_m2::InstallItemVariantBridge();
            wxl_modern_m2::InstallRetailItemAccessors();
            wxl_modern_m2::InstallCharModel();
            wxl_modern_m2::InstallRetailEquipment();
        }
        else
            api->Log(WXL_LOG_INFO, "wxl-modern-m2",
                     "retail items and 3D collections disabled by configuration");

        if (!wxl_modern_m2::InstallLegacySpellPresentation())
            api->Log(WXL_LOG_WARN, "wxl-modern-m2", "legacy spell presentation adapter unavailable; native data retained");

        if (wxl_modern_m2::ConfigBool("WXL_M2_RETAIL_SPELLS", true))
            wxl_modern_m2::InstallRetailSpellVisuals();
        else
            api->Log(WXL_LOG_INFO, "wxl-modern-m2",
                     "retail spell-model presentation disabled by configuration");
        wxl_modern_m2::InstallM2LodVariant();
    }

    api->Log(WXL_LOG_INFO, "wxl-modern-m2",
             "native M2 reader active (extension-owned retail item bridge ready)");
    return 1;
}
