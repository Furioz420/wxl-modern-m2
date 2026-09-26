// Choosing a modern race model's customization by hand, because the client cannot be asked for it.
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

// MECHANISM:
//
//   A modern appearance is a list of choices, one per option. The stock client does not keep such a
//   list: it keeps five legacy bytes, and the table that maps those onto modern choices ships no rows
//   at all. So every choice we stand on is a stand-in, and the whole appearance is one arbitrary
//   point in a space nobody can currently navigate.
//
//   That is what this panel is for. It does not guess better than the fallback -- it hands the space
//   over to be walked by hand, option by named option, and shows what each selection actually
//   contributes. Which is the only way to tell a geoset that is wrong from a texture that is wrong,
//   when both arrive together and neither was chosen on purpose.
//
//   An override replaces the stand-in for one option and nothing else. The rest keep falling back,
//   so a single piece can be moved without disturbing the appearance around it.
//
//   The geoset decision's own controls live here too, under the same window: what is drawn and what
//   was chosen are the same question asked twice, and separating them meant reading one panel to
//   understand the other.

#include "../ExtensionApi.hpp"
#include "CustomizationOwnership.hpp"
#include "GilneanPreview.hpp"
#include "GilneanChoiceMapping.hpp"

#include "game/Script.hpp"
#include "offsets/game/M2.hpp"
#include "offsets/game/World.hpp"
#include "wxl/AppearanceApi.h"

#include <cmath>
#include <cstddef>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace
{
    namespace off = wxl::offsets::game::m2;
    namespace script = wxl::game::script;

    bool IsPreviewContext()
    {
        return wxl_modern_m2::customization::IsPreviewMap(*reinterpret_cast<const int32_t*>(
            wxl::offsets::game::world::kCurrentMapId));
    }

    const WXL_AppearanceApi* g_appearance = nullptr;

    const WXL_AppearanceApi* Appearance()
    {
        if (!g_appearance)
            g_appearance = static_cast<const WXL_AppearanceApi*>(
                wxl_modern_m2::g_api->GetInterface("wxl.appearance", WXL_APPEARANCE_API_VERSION));
        return g_appearance;
    }

    /// Wide enough for every option a shipped model offers.
    constexpr uint32_t kMaxOptions = 64;
    /// Glue's stock customization ids are 1..5. Values in this private range identify a modern
    /// option by its zero-based position in the current ChrModel.
    constexpr uint32_t kGlueModernOptionBase = 1000;

    // Female Orc's Eye Style choices are relational modifiers on only the special Eye Color
    // family, not standalone materials. Retail never presents an inert style/color combination;
    // when the legacy Glue panel changes the style first, move the colour into that compatible
    // family so Slit/Star/Glow has an immediate, visible material to select.
    constexpr uint32_t kOrcEyeColorOption = 825;
    constexpr uint32_t kOrcEyeStyleOption = 8525;
    constexpr uint32_t kOrcStyledEyeFirst = 18;
    constexpr uint32_t kOrcStyledEyeLast = 31;

    /// No override: this option keeps whatever the automatic decision settles on.
    constexpr int kAutomatic = -1;

    /// The model the panel is working on, and one chosen choice INDEX per option of it. Indices and
    /// not ids, because the panel walks options by position and an index survives being read back.
    uint32_t g_model = 0;
    uint32_t g_race = 0;
    uint32_t g_sex = 0;
    uint32_t g_class = 0;
    uint32_t g_generation = 0;
    uint32_t g_legacyBrokenHairColor = 0;
    uint32_t g_legacyBrokenHornStyle = 1;
    int      g_override[kMaxOptions];
    bool     g_ready = false;
    bool     g_selectionPending = false;

    /// The last character seen composing, kept only so a change here can ask it to compose again.
    void* g_component = nullptr;

    void Forget()
    {
        for (int& slot : g_override) slot = kAutomatic;
        g_ready = true;
    }

    void RequestRebuild()
    {
        if (!IsPreviewContext()) return;
        if (!g_component) return;
        __try
        {
            *(static_cast<uint8_t*>(g_component) + off::kOffCharComponentRebuild) |=
                off::kCharRebuildSheet | off::kCharRebuildGeosets;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            g_component = nullptr;
        }
    }

    void ApplyRequiredDracthyrHorns();

    constexpr uint32_t kTuskarrCreatureSkinCount = 3;
    constexpr uint32_t kMurlocFemaleChoiceCount = 5;
    constexpr uint32_t kMurlocFemaleOptionCount = 2;

    bool IsMurlocCreatureFemale()
    {
        return g_race == 28 && g_sex == 1;
    }

    const char* MurlocChoiceName(uint32_t optionIndex, uint32_t choiceIndex)
    {
        static constexpr const char* skinNames[kMurlocFemaleChoiceCount] = {
            "Verdant", "Lagoon", "Coral", "Violet", "Pearl"
        };
        static constexpr const char* outfitNames[kMurlocFemaleChoiceCount] = {
            "Brown / Coral", "Auburn / Ocean", "Deep Sea / Kelp",
            "Seafoam / Royal", "Platinum / Ivory"
        };
        if (choiceIndex >= kMurlocFemaleChoiceCount) return "";
        return optionIndex == 0 ? skinNames[choiceIndex] : outfitNames[choiceIndex];
    }

    uint32_t MurlocChoiceSwatch(uint32_t optionIndex, uint32_t choiceIndex)
    {
        static constexpr uint32_t skin[kMurlocFemaleChoiceCount] = {
            0x65A84A, 0x27B68C, 0x2E83CC, 0x8A4BD4, 0xD7D0C5
        };
        static constexpr uint32_t outfit[kMurlocFemaleChoiceCount] = {
            0xCF3E50, 0x2B83C6, 0x3F8E3B, 0x7543AE, 0xD8D2C8
        };
        if (choiceIndex >= kMurlocFemaleChoiceCount) return 0;
        return optionIndex == 0 ? skin[choiceIndex] : outfit[choiceIndex];
    }

    bool SetMurlocFemaleChoice(uint32_t optionIndex, uint32_t choiceIndex)
    {
        if (!IsMurlocCreatureFemale() || optionIndex >= kMurlocFemaleOptionCount ||
            choiceIndex >= kMurlocFemaleChoiceCount)
            return false;
        g_override[optionIndex] = static_cast<int>(choiceIndex);
        ++g_generation;
        RequestRebuild();
        WLOG_INFO("customization-bridge: Murloc female option=%u index=%u/%u",
                  optionIndex, choiceIndex, kMurlocFemaleChoiceCount);
        return true;
    }

    bool CycleMurlocFemaleChoice(uint32_t optionIndex, int delta)
    {
        if (!IsMurlocCreatureFemale() || optionIndex >= kMurlocFemaleOptionCount || !delta)
            return false;
        const int current = g_override[optionIndex] >= 0 ? g_override[optionIndex] : 0;
        int next = (current + (delta > 0 ? 1 : -1)) %
                   static_cast<int>(kMurlocFemaleChoiceCount);
        if (next < 0) next += static_cast<int>(kMurlocFemaleChoiceCount);
        return SetMurlocFemaleChoice(optionIndex, static_cast<uint32_t>(next));
    }

    uint32_t TuskarrCreatureSkinVariant(uint32_t choiceIndex)
    {
        static constexpr uint32_t variants[] = { 1u, 2u, 7u };
        return choiceIndex < kTuskarrCreatureSkinCount ? variants[choiceIndex] : 1u;
    }

    bool IsTuskarrCreatureFemale()
    {
        return g_race == 14 && g_sex == 1;
    }

    bool SetTuskarrCreatureSkin(uint32_t choiceIndex)
    {
        if (!IsTuskarrCreatureFemale() || choiceIndex >= kTuskarrCreatureSkinCount) return false;
        g_override[0] = static_cast<int>(choiceIndex);
        ++g_generation;
        RequestRebuild();
        WLOG_INFO("customization-bridge: Tuskarr female creature skin index=%u/%u",
                  choiceIndex, kTuskarrCreatureSkinCount);
        return true;
    }

    bool CycleTuskarrCreatureSkin(int delta)
    {
        if (!IsTuskarrCreatureFemale() || !delta) return false;
        const int current = g_override[0] >= 0 ? g_override[0] : 0;
        int next = (current + (delta > 0 ? 1 : -1)) %
                   static_cast<int>(kTuskarrCreatureSkinCount);
        if (next < 0) next += static_cast<int>(kTuskarrCreatureSkinCount);
        return SetTuskarrCreatureSkin(static_cast<uint32_t>(next));
    }

    bool SelectContext(uint32_t chrRaceId, uint32_t sex)
    {
        if (!IsPreviewContext()) return false;
        uint32_t chrModel = 0;
        const uint32_t retailRace = wxl_modern_m2::RetailCharacterCanaryAllows(chrRaceId, sex)
            ? wxl_modern_m2::RetailCharacterRace(chrRaceId) : 0;
        const WXL_AppearanceApi* const tables = Appearance();
        if (retailRace && tables && tables->ChrModelForRace)
            chrModel = tables->ChrModelForRace(retailRace, chrRaceId == 14 && sex == 1 ? 0u : sex);

        const bool changed =
            chrRaceId != g_race || sex != g_sex || chrModel != g_model;
        g_race = chrRaceId;
        g_sex = sex;
        g_model = chrModel;
        if (!changed) return false;

        // The Glue character model can keep both its CMO and root-scene addresses while replacing
        // the selected sex/race. Retire the old recipe explicitly; otherwise its external horn and
        // decoration children remain visible until a full model reload.
        wxl_modern_m2::ResetCustomizationAttachments();

        // Glue can select a private race before its replacement M2 has loaded. In particular an
        // absent asset becomes errorcube, so RaceOfModelPath never gets a chance to publish the new
        // context. Clear the old race's option indices here, from the selection itself, and let the
        // later model observation confirm the same tuple.
        Forget();
        ApplyRequiredDracthyrHorns();
        g_legacyBrokenHairColor = 0;
        g_legacyBrokenHornStyle = 1;
        ++g_generation;
        g_selectionPending = true;
        RequestRebuild();
        WLOG_INFO("customization-bridge: context generation=%u race=%u retail=%u sex=%u model=%u",
                  g_generation, chrRaceId, retailRace, sex, chrModel);
        return true;
    }

    bool IsAxisOption(uint32_t axis, const char* name)
    {
        if (!name) return false;
        switch (axis)
        {
        case 1: return std::strcmp(name, "Skin Color") == 0 || std::strcmp(name, "Skin Type") == 0;
        case 2: return std::strcmp(name, "Face") == 0;
        case 3: return std::strcmp(name, "Hair Style") == 0;
        case 4: return std::strcmp(name, "Hair Color") == 0;
        case 5:
            // WotLK calls the fifth byte Facial Hair but changes its label by race and sex. Retail
            // split that byte into several named options; take the first matching legacy analogue.
            return std::strcmp(name, "Facial Hair") == 0 || std::strcmp(name, "Beard") == 0 ||
                   std::strcmp(name, "Mustache") == 0 || std::strcmp(name, "Piercings") == 0 ||
                   std::strcmp(name, "Earrings") == 0 || std::strcmp(name, "Ears") == 0 ||
                   std::strcmp(name, "FacialJewelry") == 0 || std::strcmp(name, "Tusks") == 0 ||
                   std::strcmp(name, "Horns") == 0;
        default: return false;
        }
    }

    bool HasSecondSwatch(const WXL_AppearanceApi* tables)
    {
        return tables &&
            tables->structSize >= offsetof(WXL_AppearanceApi, ChoiceSwatchColor2) +
                                  sizeof(tables->ChoiceSwatchColor2) &&
            tables->ChoiceSwatchColor2;
    }

    uint32_t SecondSwatch(const WXL_AppearanceApi* tables, uint32_t choiceId)
    {
        return HasSecondSwatch(tables) ? tables->ChoiceSwatchColor2(choiceId) : 0;
    }

    uint32_t EffectiveSwatch(const WXL_ChrChoice& choice)
    {
        if (choice.swatchColor || g_race != 2 || g_sex != 1) return choice.swatchColor;

        // Retail leaves the older Mag'har/Frostwolf skin choices without UI swatches. These previews
        // are measured from the mean non-black RGB of each choice's exact PTR base-skin BLP (resolved
        // through ChrCustomizationMaterial -> TextureFileData), rather than presenting a number or
        // inventing black. Keep the mapping choice-id based so it cannot leak to another race/model.
        switch (choice.id)
        {
        case 449: return 5657667; // orcfemaleskin00_100_hd
        case 450: return 6373681; // orcfemaleskin00_101_hd
        case 454: return 6313297; // orcfemaleskin00_102_hd
        case 455: return 6380123; // orcfemaleskin00_103_hd
        case 456: return 6050643; // orcfemaleskin00_104_hd
        case 457: return 9389627; // orcfemaleskin00_105_hd
        case 458: return 5863233; // orcfemaleskin00_106_hd
        case 459: return 7169637; // orcfemaleskin00_107_hd
        case 460: return 4996407; // orcfemaleskin00_108_hd
        case 461: return 6312523; // orcfemaleskin00_109_hd
        case 462: return 6312267; // orcfemaleskin00_110_hd
        case 463: return 6246474; // orcfemaleskin00_111_hd
        case 464: return 6503212; // orcfemaleskin00_112_hd
        case 465: return 6372139; // orcfemaleskin00_113_hd
        case 466: return 6043690; // orcfemaleskin00_114_hd
        case 467: return 5782068; // orcfemaleskin00_115_hd
        case 468: return 5979446; // orcfemaleskin00_116_hd
        case 469: return 6243387; // orcfemaleskin00_117_hd
        case 470: return 3417899; // orcfemaleskin00_118_hd
        default: return 0;
        }
    }

    bool OptionHasAnySwatch(const WXL_AppearanceApi* tables, uint32_t optionId)
    {
        if (!tables) return false;
        // Blindfold combines named geometry styles and precoloured variants in
        // one authored option; a few swatches do not make it a colour-only axis.
        const char* name = tables->OptionName ? tables->OptionName(optionId) : nullptr;
        if (name && std::strcmp(name, "Blindfold") == 0) return false;
        const uint32_t count = tables->ChoiceCount(optionId);
        for (uint32_t i = 0; i < count; ++i)
        {
            WXL_ChrChoice choice{};
            if (!tables->ChoiceAt(optionId, i, &choice)) continue;
            if (choice.swatchColor != 0 || SecondSwatch(tables, choice.id) != 0) return true;
        }
        return false;
    }

    uint32_t SecondaryOrder(const WXL_AppearanceApi* tables, uint32_t optionId)
    {
        return tables &&
            tables->structSize >= offsetof(WXL_AppearanceApi, OptionSecondaryOrderIndex) +
                                  sizeof(tables->OptionSecondaryOrderIndex) &&
            tables->OptionSecondaryOrderIndex
            ? tables->OptionSecondaryOrderIndex(optionId) : 0;
    }

    const char* FriendlyGroupName(const char* group)
    {
        if (!group) return nullptr;
        if (std::strcmp(group, "Facial1") == 0 || std::strcmp(group, "Facial2") == 0 ||
            std::strcmp(group, "Facial3") == 0 || std::strcmp(group, "Skull") == 0 ||
            std::strcmp(group, "HeadSwap") == 0)
            return "Face";
        if (std::strcmp(group, "Ears") == 0 || std::strcmp(group, "FacialJewelry") == 0 ||
            std::strcmp(group, "Piercings") == 0)
            return "Piercings";
        if (std::strcmp(group, "Eyes") == 0 || std::strcmp(group, "EyeEffects") == 0 ||
            std::strcmp(group, "EyeGlow2") == 0)
            return "Eyes";
        return group;
    }

    /// Gives the Glue a useful label even when this local DB2's Name_lang string pool is empty.
    /// SecondaryOrderIndex, not primary row position, preserves Retail's legacy face/skin/hair axes.
    /// Later options are named only when their own choice recipe identifies a geoset family.
    const char* OptionDisplayName(const WXL_AppearanceApi* tables, uint32_t optionIndex,
                                  const WXL_ChrOption& option, char* fallback, size_t fallbackSize)
    {
        // Female Tuskarr is a creature-model preview borrowing race 17's male option carrier. Its
        // first axis is deliberately repurposed to select one of the verified creature display skins.
        if (g_race == 14 && g_sex == 1 && optionIndex == 0) return "Skin Color";
        const char* name = tables->OptionName ? tables->OptionName(option.id) : nullptr;
        if (name && *name) return name;

        static const char* const legacyNames[] = {
            nullptr, "Skin Color", "Face", "Hair Style", "Hair Color", "Eye Color",
        };
        const uint32_t secondary = SecondaryOrder(tables, option.id);
        if (secondary >= 1 && secondary <= 5) return legacyNames[secondary];

        const uint32_t choiceCount = tables->ChoiceCount(option.id);
        const uint32_t inspect = choiceCount < 8 ? choiceCount : 8;
        const char* group = nullptr;
        bool hasSwatch = false;
        for (uint32_t i = 0; i < inspect; ++i)
        {
            WXL_ChrChoice choice{};
            if (!tables->ChoiceAt(option.id, i, &choice)) continue;
            hasSwatch = hasSwatch || choice.swatchColor != 0 ||
                        SecondSwatch(tables, choice.id) != 0;

            WXL_Recipe recipe{};
            const uint32_t choiceId = choice.id;
            if (!tables->BuildForCharacter(
                    wxl_modern_m2::RetailCharacterRace(g_race), g_sex,
                    &choiceId, 1, &recipe))
                continue;
            for (uint32_t g = 0; g < recipe.geosetCount; ++g)
            {
                const char* candidate = wxl_modern_m2::GeosetGroupName(recipe.geosets[g]);
                if (candidate && std::strcmp(candidate, "Skin") != 0)
                {
                    group = candidate;
                    break;
                }
            }
            for (uint32_t a = 0; !group && a < recipe.attachedCount; ++a)
            {
                const char* candidate = wxl_modern_m2::GeosetGroupName(recipe.attached[a].geoset);
                if (candidate && std::strcmp(candidate, "Skin") != 0) group = candidate;
            }
        }

        group = FriendlyGroupName(group);
        if (group)
        {
            std::snprintf(fallback, fallbackSize, "%s%s", group,
                          hasSwatch && std::strstr(group, "Color") == nullptr ? " Color" : "");
            return fallback;
        }
        std::snprintf(fallback, fallbackSize, hasSwatch ? "Color %u" : "Customization %u",
                      optionIndex + 1);
        return fallback;
    }

    uint32_t LegacyAxisForOption(const WXL_AppearanceApi* tables,
                                 const WXL_ChrOption& option, const char* name)
    {
        for (uint32_t axis = 1; axis <= 5; ++axis)
            if (IsAxisOption(axis, name)) return axis;
        const uint32_t secondary = SecondaryOrder(tables, option.id);
        if (secondary >= 1 && secondary <= 4) return secondary;
        return 0;
    }

    const char* OptionCategory(const WXL_AppearanceApi* tables,
                               const WXL_ChrOption& option, const char* name)
    {
        if (name)
        {
            // Dark Iron's Tattoo choices are face-only markings. The generic body bucket is correct
            // for several other races' body tattoos, so scope this correction to the private Dark
            // Iron client row rather than reclassifying every option named Tattoo.
            if (g_race == 29 && std::strcmp(name, "Tattoo") == 0) return "face";

            const char* const accessoryTerms[] = {
                "Accessory", "Accessories", "Blindfold", "Earring", "Jewelry",
                "Jewellery", "Piercing", "Ring", "Horn", "Headdress", "Decoration",
            };
            for (const char* term : accessoryTerms)
                if (std::strstr(name, term)) return "accessories";

            const char* const facialHairTerms[] = {
                "Facial Hair", "FacialHair", "Beard", "Mustache", "Moustache",
                "Sideburn", "Chin", "Tusk",
            };
            for (const char* term : facialHairTerms)
                if (std::strstr(name, term)) return "facial";

            const char* const hairTerms[] = {
                "Hair", "Fur Color", "Fur Style", "Quill",
            };
            for (const char* term : hairTerms)
                if (std::strstr(name, term)) return "hair";

            const char* const bodyTerms[] = {
                "Skin", "Body", "Torso", "Arm", "Leg", "Hand", "Foot", "Feet",
                "Tail", "Marking", "Tattoo", "Pattern", "Posture", "Build",
            };
            for (const char* term : bodyTerms)
                if (std::strstr(name, term)) return "body";

            const char* const faceTerms[] = {
                "Face", "Eye", "Scar", "Nose", "Ear", "Brow", "Forehead", "Jaw",
            };
            for (const char* term : faceTerms)
                if (std::strstr(name, term)) return "face";
        }
        const uint32_t secondary = SecondaryOrder(tables, option.id);
        if (secondary == 1) return "body";
        if (secondary == 2 || secondary == 5) return "face";
        if (secondary == 3 || secondary == 4) return "hair";
        return "accessories";
    }

    /** Dracthyr visage head materials require an authored horn mesh on this client. */
    void ApplyRequiredDracthyrHorns()
    {
        if ((g_race != 17 && g_race != 30) || !g_model) return;
        const WXL_AppearanceApi* const tables = Appearance();
        if (!tables) return;
        const uint32_t optionCount = tables->OptionCount(g_model);
        for (uint32_t optionIndex = 0;
             optionIndex < optionCount && optionIndex < kMaxOptions; ++optionIndex)
        {
            WXL_ChrOption option{};
            if (!tables->OptionAt(g_model, optionIndex, &option)) continue;
            const char* const optionName =
                tables->OptionName ? tables->OptionName(option.id) : nullptr;
            if (!optionName || std::strcmp(optionName, "Horns") != 0) continue;

            const uint32_t count = tables->ChoiceCount(option.id);
            for (uint32_t choiceIndex = 0; choiceIndex < count; ++choiceIndex)
            {
                WXL_ChrChoice choice{};
                if (!tables->ChoiceAt(option.id, choiceIndex, &choice)) continue;
                const char* const choiceName =
                    tables->ChoiceName ? tables->ChoiceName(choice.id) : nullptr;
                if (choiceName && std::strcmp(choiceName, "None") == 0) continue;
                if (g_override[optionIndex] != static_cast<int>(choiceIndex))
                {
                    g_override[optionIndex] = static_cast<int>(choiceIndex);
                    WLOG_INFO("customization-bridge: required Dracthyr horns option=%u index=%u",
                              option.id, choiceIndex);
                }
                return;
            }
            return;
        }
    }

    bool CycleOption(uint32_t optionIndex, int delta)
    {
        const WXL_AppearanceApi* tables = Appearance();
        if (!tables || !g_model || !delta || optionIndex >= kMaxOptions ||
            optionIndex >= tables->OptionCount(g_model))
            return false;

        WXL_ChrOption option{};
        if (!tables->OptionAt(g_model, optionIndex, &option)) return false;
        const uint32_t choiceCount = tables->ChoiceCount(option.id);
        if (!choiceCount) return false;

        const int current = g_override[optionIndex] >= 0 ? g_override[optionIndex] : 0;
        int next = wxl_modern_m2::gilnean::NextSelectable(tables, option.id, current, delta);
        if (next < 0) return false;
        g_override[optionIndex] = next;
        ApplyRequiredDracthyrHorns();

        if (option.id == kOrcEyeStyleOption)
        {
            const uint32_t optionCount = tables->OptionCount(g_model);
            for (uint32_t i = 0; i < optionCount && i < kMaxOptions; ++i)
            {
                WXL_ChrOption candidate{};
                if (!tables->OptionAt(g_model, i, &candidate) ||
                    candidate.id != kOrcEyeColorOption)
                    continue;
                const int eye = g_override[i] >= 0 ? g_override[i] : 0;
                if (eye < static_cast<int>(kOrcStyledEyeFirst) ||
                    eye > static_cast<int>(kOrcStyledEyeLast))
                {
                    g_override[i] = static_cast<int>(kOrcStyledEyeFirst);
                    WLOG_INFO("customization-bridge: Eye Style selected compatible Eye Color index=%u",
                              kOrcStyledEyeFirst);
                }
                break;
            }
        }
        ++g_generation;
        RequestRebuild();
        WLOG_INFO("customization-bridge: option=%u position=%u index=%u/%u", option.id,
                  optionIndex, static_cast<uint32_t>(next), choiceCount);
        return true;
    }

    bool SetOptionChoice(uint32_t optionIndex, uint32_t choiceIndex)
    {
        const WXL_AppearanceApi* tables = Appearance();
        if (!tables || !g_model || optionIndex >= kMaxOptions ||
            optionIndex >= tables->OptionCount(g_model))
            return false;

        WXL_ChrOption option{};
        if (!tables->OptionAt(g_model, optionIndex, &option)) return false;
        const uint32_t choiceCount = tables->ChoiceCount(option.id);
        if (!choiceCount || choiceIndex >= choiceCount) return false;
        WXL_ChrChoice selected{};
        if (!tables->ChoiceAt(option.id, choiceIndex, &selected) ||
            !wxl_modern_m2::gilnean::Selectable(tables, selected)) return false;

        g_override[optionIndex] = static_cast<int>(choiceIndex);
        ApplyRequiredDracthyrHorns();

        if (option.id == kOrcEyeStyleOption)
        {
            const uint32_t optionCount = tables->OptionCount(g_model);
            for (uint32_t i = 0; i < optionCount && i < kMaxOptions; ++i)
            {
                WXL_ChrOption candidate{};
                if (!tables->OptionAt(g_model, i, &candidate) ||
                    candidate.id != kOrcEyeColorOption)
                    continue;
                const int eye = g_override[i] >= 0 ? g_override[i] : 0;
                if (eye < static_cast<int>(kOrcStyledEyeFirst) ||
                    eye > static_cast<int>(kOrcStyledEyeLast))
                    g_override[i] = static_cast<int>(kOrcStyledEyeFirst);
                break;
            }
        }
        ++g_generation;
        RequestRebuild();
        WLOG_INFO("customization-bridge: direct option=%u position=%u index=%u/%u", option.id,
                  optionIndex, choiceIndex, choiceCount);
        return true;
    }

    uint32_t NextCustomizationRandom()
    {
        static uint32_t state = 0;
        if (!state)
            state = GetTickCount() ^
                static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&state));
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    }

    bool IsPlaceholderChoice(const WXL_AppearanceApi* tables, const WXL_ChrChoice& choice)
    {
        const char* name = tables && tables->ChoiceName ? tables->ChoiceName(choice.id) : nullptr;
        return name && std::strcmp(name, "Transmog") == 0;
    }

    bool RandomizeModernOptions()
    {
        if (IsTuskarrCreatureFemale())
            return SetTuskarrCreatureSkin(NextCustomizationRandom() % kTuskarrCreatureSkinCount);
        const WXL_AppearanceApi* tables = Appearance();
        if (!tables || !g_model) return false;

        uint32_t changed = 0;
        const uint32_t optionCount = tables->OptionCount(g_model);
        for (uint32_t optionIndex = 0;
             optionIndex < optionCount && optionIndex < kMaxOptions; ++optionIndex)
        {
            WXL_ChrOption option{};
            if (!tables->OptionAt(g_model, optionIndex, &option)) continue;
            const uint32_t count = tables->ChoiceCount(option.id);
            if (!count) continue;

            const uint32_t start = NextCustomizationRandom() % count;
            for (uint32_t offset = 0; offset < count; ++offset)
            {
                const uint32_t choiceIndex = (start + offset) % count;
                WXL_ChrChoice choice{};
                if (!tables->ChoiceAt(option.id, choiceIndex, &choice) ||
                    IsPlaceholderChoice(tables, choice))
                    continue;
                g_override[optionIndex] = static_cast<int>(choiceIndex);
                ++changed;
                break;
            }
        }

        ApplyRequiredDracthyrHorns();

        if (!changed) return false;
        ++g_generation;
        RequestRebuild();
        WLOG_INFO("customization-bridge: randomized %u modern option(s) for model=%u",
                  changed, g_model);
        return true;
    }

    bool CycleAxis(uint32_t axis, int delta)
    {
        const WXL_AppearanceApi* tables = Appearance();
        if (!tables || !g_model || !delta) return false;
        const uint32_t optionCount = tables->OptionCount(g_model);
        for (uint32_t i = 0; i < optionCount && i < kMaxOptions; ++i)
        {
            WXL_ChrOption option{};
            if (!tables->OptionAt(g_model, i, &option)) continue;
            char fallback[48];
            const char* name = OptionDisplayName(tables, i, option, fallback, sizeof fallback);
            if (LegacyAxisForOption(tables, option, name) != axis) continue;
            const uint32_t choiceCount = tables->ChoiceCount(option.id);
            if (!choiceCount) return false;
            const int current = g_override[i] >= 0 ? g_override[i] : 0;
            int next = wxl_modern_m2::gilnean::NextSelectable(tables, option.id, current, delta);
            if (next < 0) return false;
            g_override[i] = next;
            ++g_generation;
            RequestRebuild();
            WLOG_INFO("customization-bridge: axis=%u option=%u index=%u/%u", axis, option.id,
                      static_cast<uint32_t>(next), choiceCount);
            return true;
        }
        return false;
    }

    int __cdecl LuaCycleAxis(void* state)
    {
        if (!state || script::ArgCount(state) < 2 ||
            !script::IsNumber(state, 1) || !script::IsNumber(state, 2))
        {
            script::PushBoolean(state, false);
            return 1;
        }
        const double axis = script::ToNumber(state, 1);
        const double delta = script::ToNumber(state, 2);
        const bool valid = std::isfinite(axis) && std::isfinite(delta) &&
            axis >= 1.0 && axis <= 5.0 && delta != 0.0;
        script::PushBoolean(state, valid && CycleAxis(static_cast<uint32_t>(axis), delta > 0.0 ? 1 : -1));
        return 1;
    }

    int __cdecl LuaModernOptionInfo(void* state)
    {
        if (!state || script::ArgCount(state) < 1 || !script::IsNumber(state, 1))
        {
            script::PushNil(state);
            return 1;
        }

        const double ordinal = script::ToNumber(state, 1);
        if (!std::isfinite(ordinal) || ordinal < 1.0)
        {
            script::PushNil(state);
            return 1;
        }
        if (IsTuskarrCreatureFemale())
        {
            if (ordinal != 1.0)
            {
                script::PushNil(state);
                return 1;
            }
            script::PushNumber(state, static_cast<double>(kGlueModernOptionBase));
            script::PushString(state, "Skin Color");
            script::PushNumber(state, 0.0);
            script::PushNumber(state, static_cast<double>(kTuskarrCreatureSkinCount));
            script::PushString(state, "body");
            return 5;
        }
        if (IsMurlocCreatureFemale())
        {
            const uint32_t optionIndex = static_cast<uint32_t>(ordinal - 1.0);
            if (optionIndex >= kMurlocFemaleOptionCount)
            {
                script::PushNil(state);
                return 1;
            }
            script::PushNumber(state, static_cast<double>(kGlueModernOptionBase + optionIndex));
            script::PushString(state, optionIndex == 0 ? "Skin Color" : "Hair & Dress");
            script::PushNumber(state, 0.0);
            script::PushNumber(state, static_cast<double>(kMurlocFemaleChoiceCount));
            script::PushString(state, "body");
            return 5;
        }

        const WXL_AppearanceApi* tables = Appearance();
        if (!tables || !g_model)
        {
            script::PushNil(state);
            return 1;
        }
        const uint32_t optionIndex = static_cast<uint32_t>(ordinal - 1.0);
        if (optionIndex >= kMaxOptions || optionIndex >= tables->OptionCount(g_model))
        {
            script::PushNil(state);
            return 1;
        }

        WXL_ChrOption option{};
        if (!tables->OptionAt(g_model, optionIndex, &option))
        {
            script::PushNil(state);
            return 1;
        }

        char fallback[48];
        const char* name = OptionDisplayName(tables, optionIndex, option, fallback, sizeof fallback);

        script::PushNumber(state, static_cast<double>(kGlueModernOptionBase + optionIndex));
        script::PushString(state, name);
        script::PushNumber(state, static_cast<double>(LegacyAxisForOption(tables, option, name)));
        script::PushNumber(state, static_cast<double>(tables->ChoiceCount(option.id)));
        script::PushString(state, OptionCategory(tables, option, name));
        return 5;
    }

    bool OptionForGlueId(uint32_t glueId, uint32_t& optionIndex, WXL_ChrOption& option,
                         char* name, size_t nameSize)
    {
        const WXL_AppearanceApi* tables = Appearance();
        if (!tables || !g_model) return false;
        const uint32_t optionCount = tables->OptionCount(g_model);
        if (glueId >= kGlueModernOptionBase)
        {
            optionIndex = glueId - kGlueModernOptionBase;
            if (optionIndex >= kMaxOptions || optionIndex >= optionCount ||
                !tables->OptionAt(g_model, optionIndex, &option))
                return false;
            const char* display = OptionDisplayName(tables, optionIndex, option, name, nameSize);
            if (display != name) std::snprintf(name, nameSize, "%s", display);
            return true;
        }
        if (glueId < 1 || glueId > 5) return false;
        for (uint32_t i = 0; i < optionCount && i < kMaxOptions; ++i)
        {
            WXL_ChrOption candidate{};
            if (!tables->OptionAt(g_model, i, &candidate)) continue;
            char fallback[48];
            const char* display = OptionDisplayName(tables, i, candidate, fallback, sizeof fallback);
            if (LegacyAxisForOption(tables, candidate, display) != glueId) continue;
            optionIndex = i;
            option = candidate;
            std::snprintf(name, nameSize, "%s", display);
            return true;
        }
        return false;
    }

    int __cdecl LuaModernOptionState(void* state)
    {
        const WXL_AppearanceApi* tables = Appearance();
        if (!state || script::ArgCount(state) < 1 || !script::IsNumber(state, 1))
        {
            script::PushNil(state);
            return 1;
        }
        const double raw = script::ToNumber(state, 1);
        if (!std::isfinite(raw) || raw < 1.0)
        {
            script::PushNil(state);
            return 1;
        }
        if (IsTuskarrCreatureFemale() &&
            static_cast<uint32_t>(raw) == kGlueModernOptionBase)
        {
            const uint32_t selected = g_override[0] >= 0
                ? static_cast<uint32_t>(g_override[0]) : 0;
            char choiceName[32];
            std::snprintf(choiceName, sizeof choiceName, "Variant %u",
                          TuskarrCreatureSkinVariant(selected));
            script::PushString(state, "Skin Color");
            script::PushString(state, choiceName);
            script::PushNumber(state, static_cast<double>(selected + 1));
            script::PushNumber(state, static_cast<double>(kTuskarrCreatureSkinCount));
            script::PushNumber(state, 0.0);
            script::PushNumber(state, 0.0);
            script::PushBoolean(state, false);
            return 7;
        }
        if (IsMurlocCreatureFemale())
        {
            const uint32_t glueId = static_cast<uint32_t>(raw);
            const uint32_t optionIndex = glueId >= kGlueModernOptionBase
                ? glueId - kGlueModernOptionBase : kMurlocFemaleOptionCount;
            if (optionIndex >= kMurlocFemaleOptionCount)
            {
                script::PushNil(state);
                return 1;
            }
            const uint32_t selected = g_override[optionIndex] >= 0
                ? static_cast<uint32_t>(g_override[optionIndex]) : 0;
            script::PushString(state, optionIndex == 0 ? "Skin Color" : "Hair & Dress");
            script::PushString(state, MurlocChoiceName(optionIndex, selected));
            script::PushNumber(state, static_cast<double>(selected + 1));
            script::PushNumber(state, static_cast<double>(kMurlocFemaleChoiceCount));
            script::PushNumber(state, static_cast<double>(MurlocChoiceSwatch(optionIndex, selected)));
            script::PushNumber(state, 0.0);
            script::PushBoolean(state, true);
            return 7;
        }
        if (!tables)
        {
            script::PushNil(state);
            return 1;
        }

        uint32_t optionIndex = 0;
        WXL_ChrOption option{};
        char optionName[48];
        if (!OptionForGlueId(static_cast<uint32_t>(raw), optionIndex, option,
                             optionName, sizeof optionName))
        {
            script::PushNil(state);
            return 1;
        }

        const uint32_t count = tables->ChoiceCount(option.id);
        const uint32_t selected = g_override[optionIndex] >= 0
            ? static_cast<uint32_t>(g_override[optionIndex]) : 0;
        WXL_ChrChoice choice{};
        if (!count || selected >= count || !tables->ChoiceAt(option.id, selected, &choice))
        {
            script::PushNil(state);
            return 1;
        }
        const char* choiceName = tables->ChoiceName ? tables->ChoiceName(choice.id) : nullptr;
        const uint32_t swatch2 = SecondSwatch(tables, choice.id);

        script::PushString(state, optionName);
        script::PushString(state, choiceName && *choiceName ? choiceName : "");
        script::PushNumber(state, static_cast<double>(selected + 1));
        script::PushNumber(state, static_cast<double>(count));
        script::PushNumber(state, static_cast<double>(EffectiveSwatch(choice)));
        script::PushNumber(state, static_cast<double>(swatch2));
        // Keep the compact colour-grid layout when this option has real DB2 swatches anywhere in its
        // choice list. A selected choice may legitimately lack one; that cell is rendered as a number
        // instead of inventing a black colour for missing data.
        script::PushBoolean(state, OptionHasAnySwatch(tables, option.id));
        return 7;
    }

    int __cdecl LuaModernChoiceInfo(void* state)
    {
        const WXL_AppearanceApi* tables = Appearance();
        if (!state || script::ArgCount(state) < 2 ||
            !script::IsNumber(state, 1) || !script::IsNumber(state, 2))
        {
            script::PushNil(state);
            return 1;
        }
        const double rawGlueId = script::ToNumber(state, 1);
        const double rawOrdinal = script::ToNumber(state, 2);
        if (!std::isfinite(rawGlueId) || !std::isfinite(rawOrdinal) ||
            rawGlueId < 1.0 || rawOrdinal < 1.0)
        {
            script::PushNil(state);
            return 1;
        }
        if (IsTuskarrCreatureFemale() &&
            static_cast<uint32_t>(rawGlueId) == kGlueModernOptionBase)
        {
            const uint32_t choiceIndex = static_cast<uint32_t>(rawOrdinal - 1.0);
            if (choiceIndex >= kTuskarrCreatureSkinCount)
            {
                script::PushNil(state);
                return 1;
            }
            char choiceName[32];
            std::snprintf(choiceName, sizeof choiceName, "Variant %u",
                          TuskarrCreatureSkinVariant(choiceIndex));
            const uint32_t selected = g_override[0] >= 0
                ? static_cast<uint32_t>(g_override[0]) : 0;
            script::PushString(state, choiceName);
            script::PushNumber(state, 0.0);
            script::PushNumber(state, 0.0);
            script::PushBoolean(state, selected == choiceIndex);
            script::PushBoolean(state, false);
            return 5;
        }
        if (IsMurlocCreatureFemale())
        {
            const uint32_t glueId = static_cast<uint32_t>(rawGlueId);
            const uint32_t optionIndex = glueId >= kGlueModernOptionBase
                ? glueId - kGlueModernOptionBase : kMurlocFemaleOptionCount;
            const uint32_t choiceIndex = static_cast<uint32_t>(rawOrdinal - 1.0);
            if (optionIndex >= kMurlocFemaleOptionCount ||
                choiceIndex >= kMurlocFemaleChoiceCount)
            {
                script::PushNil(state);
                return 1;
            }
            const uint32_t selected = g_override[optionIndex] >= 0
                ? static_cast<uint32_t>(g_override[optionIndex]) : 0;
            script::PushString(state, MurlocChoiceName(optionIndex, choiceIndex));
            script::PushNumber(state, static_cast<double>(MurlocChoiceSwatch(optionIndex, choiceIndex)));
            script::PushNumber(state, 0.0);
            script::PushBoolean(state, selected == choiceIndex);
            script::PushBoolean(state, true);
            return 5;
        }
        if (!tables)
        {
            script::PushNil(state);
            return 1;
        }

        uint32_t optionIndex = 0;
        WXL_ChrOption option{};
        char optionName[48];
        if (!OptionForGlueId(static_cast<uint32_t>(rawGlueId), optionIndex, option,
                             optionName, sizeof optionName))
        {
            script::PushNil(state);
            return 1;
        }
        const uint32_t choiceIndex = static_cast<uint32_t>(rawOrdinal - 1.0);
        WXL_ChrChoice choice{};
        if (choiceIndex >= tables->ChoiceCount(option.id) ||
            !tables->ChoiceAt(option.id, choiceIndex, &choice))
        {
            script::PushNil(state);
            return 1;
        }

        const char* choiceName = tables->ChoiceName ? tables->ChoiceName(choice.id) : nullptr;
        const uint32_t swatch2 = SecondSwatch(tables, choice.id);
        const uint32_t selected = g_override[optionIndex] >= 0
            ? static_cast<uint32_t>(g_override[optionIndex]) : 0;
        script::PushString(state, choiceName && *choiceName ? choiceName : "");
        const uint32_t swatch1 = EffectiveSwatch(choice);
        script::PushNumber(state, static_cast<double>(swatch1));
        script::PushNumber(state, static_cast<double>(swatch2));
        script::PushBoolean(state, selected == choiceIndex);
        script::PushBoolean(state, swatch1 != 0 || swatch2 != 0);
        return 5;
    }

    int __cdecl LuaSetModernOption(void* state)
    {
        if (wxl_modern_m2::GilneanInFront()) { script::PushBoolean(state, false); return 1; }
        if (!state || script::ArgCount(state) < 2 ||
            !script::IsNumber(state, 1) || !script::IsNumber(state, 2))
        {
            script::PushBoolean(state, false);
            return 1;
        }
        const double rawGlueId = script::ToNumber(state, 1);
        const double rawOrdinal = script::ToNumber(state, 2);
        if (!std::isfinite(rawGlueId) || !std::isfinite(rawOrdinal) ||
            rawGlueId < 1.0 || rawOrdinal < 1.0)
        {
            script::PushBoolean(state, false);
            return 1;
        }
        if (IsTuskarrCreatureFemale() &&
            static_cast<uint32_t>(rawGlueId) == kGlueModernOptionBase)
        {
            script::PushBoolean(state, SetTuskarrCreatureSkin(
                static_cast<uint32_t>(rawOrdinal - 1.0)));
            return 1;
        }
        if (IsMurlocCreatureFemale())
        {
            const uint32_t glueId = static_cast<uint32_t>(rawGlueId);
            const uint32_t optionIndex = glueId >= kGlueModernOptionBase
                ? glueId - kGlueModernOptionBase : kMurlocFemaleOptionCount;
            script::PushBoolean(state, SetMurlocFemaleChoice(
                optionIndex, static_cast<uint32_t>(rawOrdinal - 1.0)));
            return 1;
        }

        uint32_t optionIndex = 0;
        WXL_ChrOption option{};
        char optionName[48];
        const bool found = OptionForGlueId(static_cast<uint32_t>(rawGlueId), optionIndex, option,
                                           optionName, sizeof optionName);
        script::PushBoolean(state, found && SetOptionChoice(
            optionIndex, static_cast<uint32_t>(rawOrdinal - 1.0)));
        return 1;
    }

    int __cdecl LuaCycleModernOption(void* state)
    {
        if (wxl_modern_m2::GilneanInFront()) { script::PushBoolean(state, false); return 1; }
        if (!state || script::ArgCount(state) < 2 ||
            !script::IsNumber(state, 1) || !script::IsNumber(state, 2))
        {
            script::PushBoolean(state, false);
            return 1;
        }
        const double glueId = script::ToNumber(state, 1);
        const double delta = script::ToNumber(state, 2);
        const bool valid = std::isfinite(glueId) && std::isfinite(delta) &&
            glueId >= static_cast<double>(kGlueModernOptionBase) &&
            glueId < static_cast<double>(kGlueModernOptionBase + kMaxOptions) &&
            delta != 0.0;
        const uint32_t optionIndex = valid
            ? static_cast<uint32_t>(glueId) - kGlueModernOptionBase
            : kMaxOptions;
        script::PushBoolean(state, valid &&
            (IsTuskarrCreatureFemale() && optionIndex == 0
                ? CycleTuskarrCreatureSkin(delta > 0.0 ? 1 : -1)
                : IsMurlocCreatureFemale() && optionIndex < kMurlocFemaleOptionCount
                ? CycleMurlocFemaleChoice(optionIndex, delta > 0.0 ? 1 : -1)
                : CycleOption(optionIndex, delta > 0.0 ? 1 : -1)));
        return 1;
    }

    int __cdecl LuaCustomizationPending(void* state)
    {
        bool pending = g_selectionPending || wxl_modern_m2::GilneanCustomizationPending();
        if (g_component && IsPreviewContext())
        {
            __try { pending = pending || ((*reinterpret_cast<const uint8_t*>(static_cast<const uint8_t*>(g_component) +
                off::kOffCharComponentRebuild) & (off::kCharRebuildSheet | off::kCharRebuildGeosets)) != 0); }
            __except (EXCEPTION_EXECUTE_HANDLER) { pending = false; }
        }
        script::PushBoolean(state, pending);
        return 1;
    }

    int __cdecl LuaCustomizationTiming(void* state)
    {
        const uint32_t now = GetTickCount();
        if (script::ArgCount(state) >= 2)
        {
            const double phase = script::ToNumber(state, 1);
            const double start = script::ToNumber(state, 2);
            if (std::isfinite(phase) && std::isfinite(start) && phase >= 1 && phase <= 4 && start >= 0 && start <= UINT32_MAX)
                WLOG_INFO("race-switch-timing: phase=%u elapsed_ms=%u race=%u generation=%u",
                    static_cast<uint32_t>(phase), now - static_cast<uint32_t>(start), g_race, g_generation);
        }
        script::PushNumber(state, now);
        return 1;
    }

    int __cdecl LuaCustomizationGeneration(void* state)
    {
        script::PushNumber(state, static_cast<double>(g_generation));
        return 1;
    }

    int __cdecl LuaCustomizationIdentity(void* state)
    {
        if (!state) return 0;
        uint32_t race = 0, sex = 0;
        if (!wxl_modern_m2::CustomizeIdentity(race, sex)) return 0;
        script::PushNumber(state, race);
        script::PushNumber(state, sex);
        return 2;
    }

    int __cdecl LuaLegacyBrokenHairColor(void* state)
    {
        if (!state || script::ArgCount(state) < 1 || !script::IsNumber(state, 1))
        {
            if (state) script::PushBoolean(state, false);
            return state ? 1 : 0;
        }
        const double raw = script::ToNumber(state, 1);
        if (!std::isfinite(raw) || raw < 0.0 || raw >= 10.0)
        {
            script::PushBoolean(state, false);
            return 1;
        }
        const uint32_t selected = static_cast<uint32_t>(raw);
        const bool changed = selected != g_legacyBrokenHairColor;
        g_legacyBrokenHairColor = selected;
        if (changed)
        {
            ++g_generation;
            RequestRebuild();
        }
        script::PushBoolean(state, true);
        return 1;
    }

    int __cdecl LuaLegacyBrokenHornStyle(void* state)
    {
        if (!state || script::ArgCount(state) < 1 || !script::IsNumber(state, 1))
        {
            if (state) script::PushBoolean(state, false);
            return state ? 1 : 0;
        }
        const double raw = script::ToNumber(state, 1);
        if (!std::isfinite(raw) || raw < 0.0 || raw >= 3.0)
        {
            script::PushBoolean(state, false);
            return 1;
        }
        const uint32_t selected = static_cast<uint32_t>(raw);
        const bool changed = selected != g_legacyBrokenHornStyle;
        g_legacyBrokenHornStyle = selected;
        if (changed)
        {
            ++g_generation;
            RequestRebuild();
        }
        script::PushBoolean(state, true);
        return 1;
    }

    int __cdecl LuaIsTuskarrCreatureFemale(void* state)
    {
        if (!state) return 0;
        script::PushBoolean(state, IsTuskarrCreatureFemale());
        return 1;
    }

    int __cdecl LuaSelectedClass(void* state)
    {
        if (!state || script::ArgCount(state) < 1 || !script::IsNumber(state, 1))
        {
            if (state) script::PushBoolean(state, false);
            return state ? 1 : 0;
        }
        const double raw = script::ToNumber(state, 1);
        if (!std::isfinite(raw) || raw < 1.0 || raw > 255.0)
        {
            script::PushBoolean(state, false);
            return 1;
        }
        const uint32_t selected = static_cast<uint32_t>(raw);
        const bool changed = selected != g_class;
        if (changed)
        {
            g_class = selected;
            RequestRebuild();
        }
        script::PushBoolean(state, changed);
        return 1;
    }

    int __cdecl LuaResetAxes(void* state)
    {
        if (!state || script::ArgCount(state) < 2) wxl_modern_m2::ResetGilneanChoices();
        if (state && script::ArgCount(state) >= 2 &&
            script::IsNumber(state, 1) && script::IsNumber(state, 2))
        {
            const double rawRace = script::ToNumber(state, 1);
            const double rawSex = script::ToNumber(state, 2);
            if (std::isfinite(rawRace) && rawRace >= 1.0 && rawRace <= 255.0 &&
                std::isfinite(rawSex) && rawSex >= 0.0 && rawSex <= 1.0)
            {
                const uint32_t race = static_cast<uint32_t>(rawRace);
                SelectContext(race,
                              static_cast<uint32_t>(rawSex));
                return 0;
            }
        }
        Forget();
        ApplyRequiredDracthyrHorns();
        g_legacyBrokenHairColor = 0;
        g_legacyBrokenHornStyle = 1;
        ++g_generation;
        RequestRebuild();
        return 0;
    }

    int __cdecl LuaRandomizeAxes(void* state)
    {
        script::PushBoolean(state, RandomizeModernOptions());
        return 1;
    }

    int __cdecl LuaDualFormInfo(void* state)
    {
        // Resolve the alternate recipe without changing the stock creation identity.
        if (!state) return 0;
        const int argc = script::ArgCount(state);
        const bool explicitRace = argc >= 2;
        if ((explicitRace && (!script::IsNumber(state, 1) || !script::IsNumber(state, 2))) ||
            (!explicitRace && (argc < 1 || !script::IsNumber(state, 1))))
            return 0;

        const double rawRace = explicitRace ? script::ToNumber(state, 1) :
                                              static_cast<double>(g_race);
        const double rawSex = script::ToNumber(state, explicitRace ? 2 : 1);
        if (!std::isfinite(rawRace) || !std::isfinite(rawSex) ||
            rawRace < 1.0 || rawRace > 255.0 || rawSex < 0.0 || rawSex > 1.0)
            return 0;

        uint32_t clientRace = static_cast<uint32_t>(rawRace);
        const uint32_t sex = static_cast<uint32_t>(rawSex);
        // The model hook is the authoritative live preview identity. Glue's selected-race value is
        // sometimes an ordinal and S_CHARACTER_RACES_INFO is keyed differently across payloads;
        // neither is allowed to suppress or misidentify this control.
        if (g_race == 12 || g_race == 17 || g_race == 30)
            clientRace = g_race;
        uint32_t retailRace = 0;
        const char* formName = nullptr;
        switch (clientRace)
        {
        case 12: retailRace = 23; formName = "Gilnean"; break;
        case 17: retailRace = 70; formName = "Dragon"; break;
        case 30: retailRace = 52; formName = "Dragon"; break;
        default: return 0;
        }

        const WXL_AppearanceApi* const tables = Appearance();
        WXL_Recipe recipe{};
        uint32_t status = 0;
        const char* modelPath = "";
        if (!tables || !tables->BuildForCharacter)
            status = 1;
        else if (!tables->BuildForCharacter(retailRace, sex, nullptr, 0, &recipe))
            status = 2;
        else if (!recipe.modelFileDataId)
            status = 3;
        else
        {
            const char* const resolved = wxl_modern_m2::ResolveModel(recipe.modelFileDataId);
            if (!resolved || !*resolved)
                status = 4;
            else
                modelPath = resolved;
        }

        // Keep eligibility independent from asset resolution. Glue must still expose the control
        // for a recognized dual-form race, while this bounded tuple trace tells us exactly which
        // DB2/model boundary failed instead of silently hiding the feature.
        static uint32_t lastClientRace = 0;
        static uint32_t lastSex = UINT32_MAX;
        static uint32_t lastRetailRace = 0;
        static uint32_t lastFileDataId = 0;
        static uint32_t lastStatus = UINT32_MAX;
        if (clientRace != lastClientRace || sex != lastSex || retailRace != lastRetailRace ||
            recipe.modelFileDataId != lastFileDataId || status != lastStatus)
        {
            WLOG_INFO("dual-form: clientRace=%u nativeRace=%u sex=%u retailRace=%u "
                      "modelFileDataId=%u status=%u path=%s",
                      clientRace, g_race, sex, retailRace, recipe.modelFileDataId, status,
                      *modelPath ? modelPath : "<unresolved>");
            lastClientRace = clientRace;
            lastSex = sex;
            lastRetailRace = retailRace;
            lastFileDataId = recipe.modelFileDataId;
            lastStatus = status;
        }

        script::PushString(state, modelPath);
        script::PushString(state, formName);
        script::PushNumber(state, static_cast<double>(retailRace));
        script::PushNumber(state, static_cast<double>(recipe.modelFileDataId));
        script::PushNumber(state, static_cast<double>(status));
        script::PushNumber(state, static_cast<double>(clientRace));
        return 6;
    }
}

namespace wxl_modern_m2
{
    void CustomizeNoteModel(uint32_t chrModel, uint32_t chrRaceId, uint32_t sex)
    {
        if (!IsPreviewContext()) return;
        if (!g_ready) Forget();
        const bool changed =
            chrRaceId != g_race || sex != g_sex || chrModel != g_model;
        g_race = chrRaceId;
        g_sex = sex;
        g_model = chrModel;
        if (!changed) return;
        // Another race/sex/model tuple: its options are not the ones these indices were picked
        // against, even when two faction rows happen to share one ChrModel.
        Forget();
        ApplyRequiredDracthyrHorns();
        ++g_generation;
    }

    void CustomizeNoteSelection()
    {
        ResetGilneanChoices();
        if (!IsPreviewContext()) return;
        // Glue reuses one component/model pointer for different characters. Race and sex therefore
        // cannot identify a new body sheet. Mark the next component observation so the newly
        // selected character, rather than the retired one, receives both rebuild bits.
        Forget();
        ++g_generation;
        g_selectionPending = true;
        WLOG_INFO("customization-bridge: character selection generation=%u pending sheet/geoset rebuild",
                  g_generation);
    }

    void CustomizeNoteComponent(void* component)
    {
        uint32_t alternateRace = 0, alternateSex = 0;
        if (GilneanPreviewIdentity(component, alternateRace, alternateSex)) return;
        // Every world CMO reaches RenderPrep. None owns the singleton Glue selection, and
        // observing one must not invalidate another CMO or reset all attachment recipes.
        if (!IsPreviewContext())
        {
            g_component = nullptr;
            g_selectionPending = false;
            static bool logged = false;
            if (!logged)
            {
                logged = true;
                WLOG_INFO("customization-owner-v1: world components isolated from Glue rebuild state");
            }
            return;
        }
        g_component = component;
        if (!component) return;
        // Component identity is available before a replacement model publishes a table recipe.
        // Private creature races have no Retail ChrModel, so without this observation the panel can
        // retain the previous playable race's option list indefinitely.
        __try
        {
            const auto* const bytes = static_cast<const uint8_t*>(component);
            const uint32_t race = *reinterpret_cast<const uint32_t*>(
                bytes + off::kOffCharComponentRace);
            const uint32_t sex = *reinterpret_cast<const uint32_t*>(
                bytes + off::kOffCharComponentSex);
            if (race && sex <= 1) SelectContext(race, sex);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        if (!g_selectionPending) return;
        __try
        {
            *(static_cast<uint8_t*>(component) + off::kOffCharComponentRebuild) |=
                off::kCharRebuildSheet | off::kCharRebuildGeosets;
            g_selectionPending = false;
            WLOG_INFO("customization-bridge: applied selection rebuild component=%p", component);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            g_component = nullptr;
        }
    }

    void* CustomizeComponent()
    {
        return IsPreviewContext() ? g_component : nullptr;
    }

    bool CustomizeIdentity(uint32_t& chrRaceId, uint32_t& sex)
    {
        if (!IsPreviewContext()) return false;
        // The root CMO is updated before its replacement model is requested. Prefer that live tuple:
        // the model-observation globals are published later and may still describe the prior race
        // while a shared ChrRaces path is being resolved.
        if (g_component)
        {
            __try
            {
                const auto* const bytes = static_cast<const uint8_t*>(g_component);
                const uint32_t liveRace = *reinterpret_cast<const uint32_t*>(
                    bytes + off::kOffCharComponentRace);
                const uint32_t liveSex = *reinterpret_cast<const uint32_t*>(
                    bytes + off::kOffCharComponentSex);
                if (liveRace && liveSex <= 1)
                {
                    chrRaceId = liveRace;
                    sex = liveSex;
                    return true;
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        if (!g_race || g_sex > 1) return false;
        chrRaceId = g_race;
        sex = g_sex;
        return true;
    }

    uint32_t CustomizeClass()
    {
        return g_class;
    }

    uint32_t CustomizeGeneration()
    {
        return g_generation;
    }

    uint32_t LegacyBrokenHairColor()
    {
        return g_legacyBrokenHairColor;
    }

    uint32_t LegacyBrokenHornStyle()
    {
        return g_legacyBrokenHornStyle;
    }

    uint32_t MurlocFemaleSkin()
    {
        return g_override[0] >= 0 ? static_cast<uint32_t>(g_override[0]) : 0;
    }

    uint32_t MurlocFemaleOutfit()
    {
        return g_override[1] >= 0 ? static_cast<uint32_t>(g_override[1]) : 0;
    }

    bool SetLinkedWorgenChoice(uint32_t model, uint32_t index, uint32_t choice)
    {
        return IsPreviewContext() && g_ready && g_model == model &&
            (model == 43 || model == 44 || model == 127 || model == 128) && SetOptionChoice(index, choice);
    }

    int CustomizeChoiceFor(uint32_t chrModel, uint32_t optionIndex)
    {
        const int alternate = GilneanPreviewChoice(chrModel, optionIndex);
        if (alternate >= 0) return alternate;
        if (!IsPreviewContext()) return kAutomatic;
        if (!g_ready || chrModel != g_model || optionIndex >= kMaxOptions) return kAutomatic;
        return g_override[optionIndex];
    }
}

namespace
{
    /// Choices one option offers. Past the widest shipped list, which is a colour ramp; an option
    /// holding more has its tail left out rather than being indexed past what the tables report.
    constexpr uint32_t kMaxChoicesListed = 128;
    /// One dropdown entry, "<order>  <name>". The longest shipped choice name sits well inside this.
    constexpr int kChoiceLabelLen = 64;
    /// One "isolate" dropdown entry, "geoset <id>(<GroupName>)". Wide enough for the longest group
    /// name ("HairDecoration"/"HornDecoration") without truncating.
    constexpr int kGeosetLabelLen = 48;
    /// As many ids as the decision can leave standing, since the list is copied out whole.
    constexpr uint32_t kMaxShownGeosets = 64;
    /// Geoset ids per line: one per line turns an ordinary character into a screen of scrolling.
    constexpr uint32_t kGeosetsPerLine = 12;

    /**
     * @brief Appends to a bounded line, keeping the cursor inside it.
     *
     * snprintf reports what it WOULD have written, so carrying that forward across a truncation makes
     * the next call's remaining-space argument negative.
     */
    void Append(char* buf, int size, int& at, const char* fmt, ...)
    {
        if (!buf || at >= size - 1) return;
        va_list args;
        va_start(args, fmt);
        const int written = std::vsnprintf(buf + at, size_t(size - at), fmt, args);
        va_end(args);
        at = (written < 0 || at + written >= size) ? size - 1 : at + written;
    }

    /// The choice id one option offers at a position, or 0 when it offers none there.
    uint32_t ChoiceIdAt(const WXL_AppearanceApi* tables, uint32_t optionId, uint32_t index)
    {
        WXL_ChrChoice choice{};
        return tables->ChoiceAt(optionId, index, &choice) ? choice.id : 0;
    }

    /**
     * @brief What one option's selection puts on the character: its geosets, and the files its layers
     *        paint into the slot the sheet is.
     *
     * Built against the appearance as a whole with only this option's choice swapped in, not against
     * the choice alone: an element can wait on a SECOND choice, so a choice judged by itself reports
     * almost nothing it would really contribute.
     */
    void DescribeSelection(const WXL_AppearanceApi* tables, uint32_t optionIndex, uint32_t optionId,
                           uint32_t choiceIndex)
    {
        const WXL_Api* api = wxl_modern_m2::g_api;
        const WXL_FdidApi* files = wxl_modern_m2::Fdid();

        uint32_t choices[kMaxOptions];
        uint32_t count = wxl_modern_m2::SettledChoicesFor(wxl_modern_m2::CustomizeComponent(), g_model, choices, kMaxOptions);
        const uint32_t taken = ChoiceIdAt(tables, optionId, choiceIndex);
        if (!taken)
        {
            api->UiText("    this option offers no choice there");
            return;
        }

        // The settled list is in option order, so the entry to replace is the one this option owns.
        if (optionIndex < count) choices[optionIndex] = taken;
        else if (count < kMaxOptions) choices[count++] = taken;

        WXL_Recipe recipe{};
        if (!tables->BuildForCharacter(wxl_modern_m2::RetailCharacterRace(g_race),
                                       g_sex, choices, count, &recipe))
        {
            api->UiText("    no recipe could be built for this choice");
            return;
        }

        constexpr int kLineLen = 384;
        char line[kLineLen];
        int at = 0;
        Append(line, kLineLen, at, "    geosets");
        for (uint32_t i = 0; i < recipe.geosetCount; ++i)
        {
            const uint32_t id = recipe.geosets[i];
            const char* groupName = wxl_modern_m2::GeosetGroupName(id);
            if (groupName) Append(line, kLineLen, at, " %u(%s)", id, groupName);
            else           Append(line, kLineLen, at, " %u", id);
        }
        if (!recipe.geosetCount) Append(line, kLineLen, at, " none");

        // Only the slot the sheet IS: the others are whole textures in other slots and say nothing
        // about what this composition will look like.
        for (uint32_t i = 0; i < recipe.layerCount; ++i)
        {
            const WXL_TextureLayer& layer = recipe.layers[i];
            if (layer.textureType != 1) continue;
            const char* const path = files
                ? files->ResolveMaterialTexture(layer.materialResourceId, layer.textureType)
                : nullptr;
            if (path) Append(line, kLineLen, at, "  |  %s", path);
        }
        api->UiText(line);
    }

    /// One option as a named dropdown over its named choices, with what the selection contributes.
    void DrawOption(const WXL_AppearanceApi* tables, uint32_t index, const WXL_ChrOption& option)
    {
        const WXL_Api* api = wxl_modern_m2::g_api;

        const uint32_t offered = tables->ChoiceCount(option.id);
        if (!offered) return;
        const uint32_t listed = offered < kMaxChoicesListed ? offered : kMaxChoicesListed;

        // Two options of one model can carry the same display name, and two controls sharing a label
        // share their state as well; the id after "##" separates them without being shown.
        char label[80];
        const char* const name = tables->OptionName ? tables->OptionName(option.id) : nullptr;
        if (name && *name) std::snprintf(label, sizeof label, "%s##%u", name, option.id);
        else               std::snprintf(label, sizeof label, "option %u##%u", option.id, option.id);

        char text[kMaxChoicesListed][kChoiceLabelLen];
        const char* items[kMaxChoicesListed];
        for (uint32_t i = 0; i < listed; ++i)
        {
            int at = 0;
            WXL_ChrChoice choice{};
            if (tables->ChoiceAt(option.id, i, &choice))
            {
                // A colour swatch carries no name at all, so the order the tables list it in is the
                // only thing that tells two of them apart.
                const char* const choiceName = tables->ChoiceName ? tables->ChoiceName(choice.id) : nullptr;
                Append(text[i], kChoiceLabelLen, at, "%u  %s", choice.orderIndex,
                       choiceName ? choiceName : "");
            }
            else Append(text[i], kChoiceLabelLen, at, "%u  (unreadable)", i);
            items[i] = text[i];
        }

        // The automatic rule takes the option's first choice, so that is what an option with no
        // override of its own is actually wearing.
        int selected = g_override[index] != kAutomatic ? g_override[index] : 0;
        if (api->UiCombo(label, &selected, items, int(listed)))
        {
            g_override[index] = selected;
            ++g_generation;
            // A change is only a change once the geometry is decided again, and the geometry is owed
            // its own bit: asking for the sheet instead does not merely fail to redecide the pieces,
            // it holds the decision off until the sheet is done. Done here rather than on a button, so
            // what is on screen answers the dropdown. The sheet follows on its own, because the choice
            // this changed is the same one its layers are read from.
            if (g_component)
                *(static_cast<uint8_t*>(g_component) + off::kOffCharComponentRebuild) |=
                    off::kCharRebuildGeosets;
        }
        DescribeSelection(tables, index, option.id, uint32_t(selected));
    }

    /// The ids the last decision left standing, a line at a time.
    void DrawShownGeosets(const uint16_t* shown, uint32_t count)
    {
        const WXL_Api* api = wxl_modern_m2::g_api;
        if (!count)
        {
            api->UiText("nothing has been decided yet");
            return;
        }

        constexpr int kLineLen = 192;
        char line[kLineLen];
        int at = 0;
        uint32_t onLine = 0;
        for (uint32_t i = 0; i < count; ++i)
        {
            const char* groupName = wxl_modern_m2::GeosetGroupName(shown[i]);
            if (groupName) Append(line, kLineLen, at, "%s%u(%s)", onLine ? " " : "", shown[i], groupName);
            else           Append(line, kLineLen, at, "%s%u", onLine ? " " : "", shown[i]);
            if (++onLine < kGeosetsPerLine && i + 1 < count) continue;
            api->UiText(line);
            at = 0;
            onLine = 0;
        }
    }

    /**
     * @brief Looking at one piece of the model on its own.
     *
     * Bisecting by editing a file and relaunching costs a minute per candidate and loses the scene
     * every time; the same walk done here costs a click and keeps the camera where it was. Only the
     * ids that are currently drawn are offered, because a geoset this model does not carry tells
     * nobody anything.
     */
    void DrawIsolation(const uint16_t* shown, uint32_t count)
    {
        const WXL_Api* api = wxl_modern_m2::g_api;
        const uint32_t isolated = wxl_modern_m2::IsolatedGeoset();
        char line[128];

        if (isolated == wxl_modern_m2::kNoIsolation)
            api->UiText("Showing: the normal decision");
        else if (isolated == wxl_modern_m2::kHideEverything)
            api->UiText("Showing: nothing at all");
        else
        {
            const char* groupName = wxl_modern_m2::GeosetGroupName(isolated);
            if (groupName) std::snprintf(line, sizeof line, "Showing: geoset %u(%s) alone", isolated, groupName);
            else           std::snprintf(line, sizeof line, "Showing: geoset %u alone", isolated);
            api->UiText(line);
        }

        if (count)
        {
            char text[kMaxShownGeosets][kGeosetLabelLen];
            const char* items[kMaxShownGeosets];
            int pick = 0;
            for (uint32_t i = 0; i < count; ++i)
            {
                const char* groupName = wxl_modern_m2::GeosetGroupName(shown[i]);
                if (groupName) std::snprintf(text[i], sizeof text[i], "geoset %u(%s)", shown[i], groupName);
                else           std::snprintf(text[i], sizeof text[i], "geoset %u", shown[i]);
                items[i] = text[i];
                if (uint32_t(shown[i]) == isolated) pick = int(i);
            }
            if (api->UiCombo("show alone", &pick, items, int(count)))
                wxl_modern_m2::SetIsolatedGeoset(shown[pick]);
        }

        if (api->UiButton("show everything (normal decision)"))
            wxl_modern_m2::SetIsolatedGeoset(wxl_modern_m2::kNoIsolation);
        api->UiSameLine();
        if (api->UiButton("hide everything"))
            wxl_modern_m2::SetIsolatedGeoset(wxl_modern_m2::kHideEverything);

        api->UiText("A change takes effect the next time the geoset decision runs:");
        api->UiText("move, or target and untarget, to force it.");
    }

    /**
     * @brief Narrowing the base body down to the one triangle that carries the spike.
     *
     * Everything the body's draw is HANDED has been measured and came back clean, so the artifact is
     * not findable by measuring an input any more. What is left is to halve what is DRAWN: the half
     * whose removal takes the spike with it holds it, and a range of one names it. Done here for the
     * same reason the isolation above is -- a bisection by editing a file costs a relaunch per step and
     * loses the pose, which is fifteen steps for a section of a thousand triangles.
     */
    void DrawBisection()
    {
        const WXL_Api* api = wxl_modern_m2::g_api;
        const uint32_t total = wxl_modern_m2::BisectTriangleCount();
        char line[160];

        if (!total)
        {
            api->UiText("No base-body draw has been seen yet.");
            api->UiText("Look at a character of a reworked race first.");
            return;
        }

        int armed = wxl_modern_m2::BisectArmed() ? 1 : 0;
        if (api->UiCheckbox("draw only this range", &armed))
            wxl_modern_m2::SetBisectArmed(armed != 0);

        // Held on the draw side, so what the sliders show is what the body is actually drawing rather
        // than a second copy of it that can drift.
        uint32_t held = 0, wanted = 0;
        wxl_modern_m2::BisectRange(held, wanted);
        int first = int(held < total ? held : total - 1);
        int count = int(wanted < total - uint32_t(first) ? wanted : total - uint32_t(first));

        bool moved = api->UiSliderInt("first triangle", &first, 0, int(total) - 1) != 0;
        if (count > int(total) - first) count = int(total) - first;
        moved = api->UiSliderInt("how many", &count, 0, int(total) - first) != 0 || moved;

        // Halving on a slider of a thousand is a dozen careful drags; the search itself is two buttons.
        if (api->UiButton("halve the count")) { count = count > 1 ? count / 2 : 1; moved = true; }
        api->UiSameLine();
        if (api->UiButton("the whole section")) { first = 0; count = int(total); moved = true; }
        if (moved) wxl_modern_m2::SetBisectRange(uint32_t(first), uint32_t(count));

        std::snprintf(line, sizeof line, "drawing triangles [%d, %d) of %u", first, first + count,
                      total);
        api->UiText(line);

        if (api->UiButton("report this range")) wxl_modern_m2::RequestBisectReport();
        api->UiText("The report names at most a handful of triangles, tagged m2-triangle,");
        api->UiText("with the longest edge of each: the spike is the long one.");
    }

    /**
     * @brief The whole customization of one model, as one named dropdown per option.
     *
     * A dropdown per option rather than a walk along an index, because the question this has to answer
     * is which of a race's actual named looks is on the character -- and an index into a table nobody
     * has the names of answers no part of it.
     */
    void __cdecl DrawPanel(void*)
    {
        const WXL_Api* api = wxl_modern_m2::g_api;
        const WXL_AppearanceApi* tables = Appearance();
        char line[256];

        if (!tables || !g_model)
        {
            api->UiText("No modern race model has been decided yet.");
            api->UiText("Look at a character of a reworked race first.");
            return;
        }

        // The model the recipe names, not the one the client asked for: the HD switch answers a stock
        // name with another file, and only the recipe settles whether the tables and the geometry are
        // talking about the same model.
        uint32_t settled[kMaxOptions];
        const uint32_t settledCount = wxl_modern_m2::SettledChoicesFor(wxl_modern_m2::CustomizeComponent(), g_model, settled, kMaxOptions);
        WXL_Recipe recipe{};
        const char* served = tables->BuildForCharacter(
                                 wxl_modern_m2::RetailCharacterRace(g_race), g_sex,
                                 settled, settledCount, &recipe)
            ? wxl_modern_m2::ResolveModel(recipe.modelFileDataId)
            : nullptr;

        std::snprintf(line, sizeof line, "race %u, sex %u, model %u", g_race, g_sex, g_model);
        api->UiText(line);
        std::snprintf(line, sizeof line, "serving %s", served ? served : "(unresolved)");
        api->UiText(line);
        api->UiSeparator();

        const uint32_t options = tables->OptionCount(g_model);
        if (api->UiCollapsingHeader("Customization"))
        {
            for (uint32_t i = 0; i < options && i < kMaxOptions; ++i)
            {
                WXL_ChrOption option{};
                if (tables->OptionAt(g_model, i, &option)) DrawOption(tables, i, option);
            }
        }

        uint16_t shown[kMaxShownGeosets];
        const uint32_t shownCount = wxl_modern_m2::ShownGeosets(shown, kMaxShownGeosets);

        if (api->UiCollapsingHeader("On the character now")) DrawShownGeosets(shown, shownCount);
        if (api->UiCollapsingHeader("Isolate one piece")) DrawIsolation(shown, shownCount);
        if (api->UiCollapsingHeader("Bisect the body triangles")) DrawBisection();

        api->UiSeparator();
        if (api->UiButton("reset every option"))
        {
            Forget();
            ApplyRequiredDracthyrHorns();
        }
        api->UiSameLine();
        // Both kinds of work, because this button means "redo it all" and not "redo the part I can
        // name". Recomposing a sheet the choices did not move is the point of asking by hand: it is
        // how a composition that came out wrong once gets another attempt.
        if (api->UiButton("rebuild this character now") && g_component)
            *(static_cast<uint8_t*>(g_component) + off::kOffCharComponentRebuild) |=
                off::kCharRebuildSheet | off::kCharRebuildGeosets;
    }

    constexpr char kGlueCustomizationBridge[] = R"lua(
do
    local owner = _G.WXLModernM2CustomizationBridge or {}
    _G.WXLModernM2CustomizationBridge = owner
    owner.extraBase = 1000
    owner.nativeCycle = _WXL_M2_CUSTOMIZATION_CYCLE
    owner.nativeInfo = _WXL_M2_CUSTOMIZATION_OPTION
    owner.nativeState = _WXL_M2_CUSTOMIZATION_STATE
    owner.nativeChoice = _WXL_M2_CUSTOMIZATION_CHOICE
    owner.nativeSetOption = _WXL_M2_CUSTOMIZATION_OPTION_SET
    owner.nativeCycleOption = _WXL_M2_CUSTOMIZATION_OPTION_CYCLE
    owner.nativeGeneration = _WXL_M2_CUSTOMIZATION_GENERATION
    owner.nativePending = _WXL_M2_CUSTOMIZATION_PENDING
    owner.nativeTiming = _WXL_M2_CUSTOMIZATION_TIMING
    owner.nativeIdentity = _WXL_M2_CUSTOMIZATION_IDENTITY
    owner.nativeBrokenHairColor = _WXL_M2_CUSTOMIZATION_BROKEN_HAIR_COLOR
    owner.nativeBrokenHornStyle = _WXL_M2_CUSTOMIZATION_BROKEN_HORN_STYLE
    owner.nativeTuskarrCreatureFemale = _WXL_M2_CUSTOMIZATION_TUSKARR_CREATURE_FEMALE
    owner.nativeClass = _WXL_M2_CUSTOMIZATION_CLASS
    owner.nativeReset = _WXL_M2_CUSTOMIZATION_RESET
    owner.nativeRandomize = _WXL_M2_CUSTOMIZATION_RANDOMIZE
    owner.nativeDualForm = _WXL_M2_DUAL_FORM_INFO
    local function L(key, fallback)
        local value = _G[key]
        if value and value ~= "" then return value end
        return fallback
    end
    local optionLocaleKeys = {
        ["Skin Color"] = "WXL_CC_OPTION_SKIN_COLOR",
        ["Skin Type"] = "WXL_CC_OPTION_SKIN_TYPE",
        ["Face"] = "WXL_CC_OPTION_FACE",
        ["Hair Style"] = "WXL_CC_OPTION_HAIR_STYLE",
        ["Hair Color"] = "WXL_CC_OPTION_HAIR_COLOR",
        ["Eye Color"] = "WXL_CC_OPTION_EYE_COLOR",
        ["Underclothes Bottom"] = "WXL_CC_OPTION_UNDERCLOTHES_BOTTOM",
        ["Underclothes Top"] = "WXL_CC_OPTION_UNDERCLOTHES_TOP",
        ["Underclothes Color"] = "WXL_CC_OPTION_UNDERCLOTHES_COLOR",
        ["Horns"] = "WXL_CC_OPTION_HORNS",
        ["Horn Style"] = "WXL_CC_OPTION_HORN_STYLE",
        ["Horn Color"] = "WXL_CC_OPTION_HORN_COLOR",
        ["Horn Jewelry"] = "WXL_CC_OPTION_HORN_JEWELRY",
        ["Jewelry Color"] = "WXL_CC_OPTION_JEWELRY_COLOR",
        ["Tendrils"] = "WXL_CC_OPTION_TENDRILS",
        ["Hair & Dress"] = "WXL_CC_OPTION_HAIR_DRESS",
    }
    local choiceLocaleKeys = {
        ["None"] = "WXL_CC_VALUE_NONE",
        ["Swept"] = "WXL_CC_VALUE_SWEPT",
    }
    function owner:LocalizeOption(name)
        local key = name and optionLocaleKeys[name]
        return key and L(key, name) or name
    end
    function owner:LocalizeChoice(name)
        local key = name and choiceLocaleKeys[name]
        return key and L(key, name) or name
    end
    local function selectedClientRace()
        local selected = C_CharacterCreation.GetSelectedRace()
        local info = S_CHARACTER_RACES_INFO and S_CHARACTER_RACES_INFO[selected]
        return (info and info.dbcRaceID) or selected
    end
    local function safeModelCall(model, method, ...)
        local callable = model and model[method]
        if type(callable) ~= "function" then return false end
        return pcall(callable, model, ...)
    end
    function owner:EnsureDualFormPreview(needModel)
        local root = _G.CharacterCreate
        local customize = root and root.GenderFrame and root.GenderFrame.CustomizationButton
        if not root or not customize then return false end

        if not self.dualFormButton then
            local button = CreateFrame("Button", "WXLDualFormPreviewButton", root,
                "UIPanelButtonTemplate")
            button:SetSize(170, 24)
            button:SetPoint("BOTTOM", customize, "TOP", 0, 16)
            button:SetFrameStrata("DIALOG")
            button:SetScript("OnClick", function()
                PlaySound("gsCharacterCreationLook")
                owner.dualFormVisible = not owner.dualFormVisible
                owner:RefreshDualFormPreview(true)
            end)
            button:SetScript("OnEnter", function(self)
                if GlueTooltip then
                    GlueTooltip:SetOwner(self, "ANCHOR_RIGHT")
                    GlueTooltip:SetText("Dual Form Preview", 1, 1, 1)
                    GlueTooltip:AddLine(
                        "Shows the alternate form beside the playable preview without changing race or sex.",
                        0.8, 0.8, 0.8)
                    GlueTooltip:Show()
                end
            end)
            button:SetScript("OnLeave", function()
                if GlueTooltip then GlueTooltip:Hide() end
            end)
            self.dualFormButton = button
        end

        if needModel and not self.dualFormModel then
            -- PlayerModel assumes a resident unit/CMO owner. Character selection has neither for
            -- this secondary frame, and clearing that empty owner dereferences null in 0x004E0A30.
            -- A plain Model owns only the explicitly loaded M2 and is created lazily on the click.
            local ok, model = pcall(CreateFrame, "Model",
                "WXLDualFormPreviewModel", root)
            if not ok or not model then return false end
            model:SetSize(360, 540)
            model:SetPoint("BOTTOM", root, "BOTTOM", 250, 105)
            model:SetFrameStrata("MEDIUM")
            model:EnableMouse(true)
            safeModelCall(model, "SetCamera", 0)
            safeModelCall(model, "SetLight", 1, 0, 0, -0.707, -0.707, 0.7,
                1.0, 1.0, 1.0, 0.8, 1.0, 1.0, 0.8)
            safeModelCall(model, "SetFacing", 0)
            safeModelCall(model, "SetSequence", 0)
            model:SetScript("OnMouseDown", function(self, mouseButton)
                if mouseButton == "LeftButton" then
                    self.WXLDragX = GetCursorPosition()
                    local okFacing, facing = pcall(self.GetFacing, self)
                    self.WXLDragFacing = okFacing and facing or 0
                end
            end)
            model:SetScript("OnMouseUp", function(self)
                self.WXLDragX = nil
                self.WXLDragFacing = nil
            end)
            model:SetScript("OnUpdate", function(self)
                if not self.WXLDragX then return end
                local x = GetCursorPosition()
                safeModelCall(self, "SetFacing",
                    (self.WXLDragFacing or 0) + (x - self.WXLDragX) * 0.01)
            end)
            model:Hide()
            self.dualFormModel = model

            local label = root:CreateFontString(nil, "OVERLAY", "GameFontNormalLarge")
            label:SetPoint("BOTTOM", model, "TOP", 0, -18)
            label:SetTextColor(1, 0.82, 0, 1)
            label:Hide()
            self.dualFormLabel = label
        end
        return true
    end

    function owner:HideDualFormPreview(clear)
        if self.dualFormModel then
            self.dualFormModel:Hide()
            if clear and self.dualFormLoaded then
                safeModelCall(self.dualFormModel, "ClearModel")
                self.dualFormLoaded = false
            end
        end
        if self.dualFormLabel then self.dualFormLabel:Hide() end
    end

    function owner:RefreshDualFormPreview(force)
        local pairedRace = _WXL_GILNEAN_CONTEXT and _WXL_GILNEAN_CONTEXT()
        if pairedRace == 12 or pairedRace == 17 or pairedRace == 30 then
            if self.dualFormButton then self.dualFormButton:Hide() end
            self:HideDualFormPreview(true)
            return
        end
        local root = _G.CharacterCreate
        local inCreation = false
        if C_CharacterCreation and C_CharacterCreation.IsInCharacterCreate then
            local ok, active = pcall(C_CharacterCreation.IsInCharacterCreate)
            inCreation = ok and active or false
        end
        local rootVisible = root and root.IsShown and root:IsShown()
        if not root or not (inCreation or rootVisible) then
            if self.dualFormButton then self.dualFormButton:Hide() end
            self.dualFormVisible = false
            self:HideDualFormPreview(true)
            self.dualFormKey = nil
            self.dualFormSelectionKey = nil
            return
        end
        local customization = root and root.CustomizationFrame
        local sex = C_CharacterCreation.GetSelectedSex()
        local path, formName, retailRace, fileDataID, resolutionStatus, race
        if self.nativeDualForm then
            path, formName, retailRace, fileDataID, resolutionStatus, race =
                self.nativeDualForm(sex)
        end
        local eligible = formName and retailRace
        if not eligible then
            if self.dualFormButton then self.dualFormButton:Hide() end
            self.dualFormVisible = false
            self:HideDualFormPreview(true)
            self.dualFormKey = nil
            self.dualFormSelectionKey = nil
            return
        end
        if not self:EnsureDualFormPreview(self.dualFormVisible) then return end

        local key = tostring(race) .. ":" .. tostring(sex) .. ":" ..
            tostring(retailRace) .. ":" .. tostring(fileDataID)
        if self.dualFormSelectionKey ~= key then
            self.dualFormSelectionKey = key
            self.dualFormVisible = false
            self:HideDualFormPreview(true)
            self.dualFormKey = nil
        end
        self.dualFormButton:SetText((self.dualFormVisible and "Hide " or "Show ") ..
            formName .. " Form")
        self.dualFormButton:Show()

        local modelResolved = path and path ~= "" and (resolutionStatus or 0) == 0
        if not modelResolved then
            self.dualFormVisible = false
            self:HideDualFormPreview(false)
            return
        end

        -- The stock customization panel owns the right side and changes the primary camera.
        -- Keep the alternate preview out of that mode without discarding the user's toggle.
        if not self.dualFormVisible or (customization and customization.show) then
            self:HideDualFormPreview(false)
            return
        end

        if force or self.dualFormKey ~= key then
            self.dualFormKey = key
            if self.dualFormLoaded then
                safeModelCall(self.dualFormModel, "ClearModel")
                self.dualFormLoaded = false
            end
            self.dualFormLoaded = safeModelCall(self.dualFormModel, "SetModel", path)
            safeModelCall(self.dualFormModel, "SetCamera", 0)
            safeModelCall(self.dualFormModel, "SetPosition", 0, 0, -1.0)
            safeModelCall(self.dualFormModel, "SetModelScale", race == 12 and 0.78 or 0.62)
            safeModelCall(self.dualFormModel, "SetFacing", 0)
            safeModelCall(self.dualFormModel, "SetSequence", 0)
        end
        self.dualFormLabel:SetText(formName .. " Form")
        self.dualFormLabel:Show()
        self.dualFormModel:Show()
    end
    local function isTuskarrCreatureFemale()
        return owner.nativeTuskarrCreatureFemale and owner.nativeTuskarrCreatureFemale() or false
    end
    local function isMurloc()
        local race = owner.selectedClientRace
        if not race then race = selectedClientRace() end
        return race == 28
    end
    local function isMurlocFemale()
        return isMurloc() and C_CharacterCreation.GetSelectedSex() == 1
    end
    local function isLegacyBroken()
        -- Glue selection ordinals and private client race IDs differ. The observed
        -- native component is authoritative once loaded, including legacy actors.
        local race = owner.nativeIdentity and owner.nativeIdentity()
        if not race then race = selectedClientRace() end
        return race == 23
    end
    owner.legacyBrokenSelection = owner.legacyBrokenSelection or {
        male = { 1, 1, 1, 1, 1 },
        female = { 1, 1, 1, 1, 1, 2 },
    }
    local legacyBrokenCounts = {
        male = { 17, 20, 14, 10, 8 },
        female = { 15, 20, 16, 10, 6, 3 },
    }
    local legacyBrokenSkinSwatches = {
        0xDCEEFF, 0xC8DEFA, 0xB4CEF2, 0x9DBCE8, 0x88A9DC, 0x7697CF,
        0x6585BE, 0x5874AB, 0x4C6398, 0x405486, 0x354674, 0x2D3B64,
        0x253253, 0x202B48, 0x1B253D, 0x171F34, 0x12192B,
    }
    local legacyBrokenHairSwatches = {
        0x81222C, 0x63362A, 0x3E2A37, 0x5C3E2D, 0x843109,
        0xA0A07E, 0x38323B, 0x4C3570, 0x25567E, 0x345539,
    }
    local function legacyBrokenKey()
        -- Native CMO sex is 0/1. Glue GetSelectedSex maps those to 2/3.
        -- Prefer the live identity, and use the actual Glue convention before a model exists.
        if owner.nativeIdentity then
            local race, sex = owner.nativeIdentity()
            if race == 23 and (sex == 0 or sex == 1) then
                return sex == 1 and "female" or "male"
            end
        end
        return C_CharacterCreation.GetSelectedSex() == 3 and "female" or "male"
    end
    local function legacyBrokenState(axis)
        local key = legacyBrokenKey()
        local count = legacyBrokenCounts[key][axis] or 1
        -- The last three private skin rows each ship only the three matching face textures.
        if axis == 2 then
            local skin = owner.legacyBrokenSelection[key][1] or 1
            local compactFaceStart = key == "female" and 13 or 15
            if skin >= compactFaceStart then count = 3 end
        end
        local selected = owner.legacyBrokenSelection[key][axis] or 1
        if selected < 1 or selected > count then selected = 1 end
        return selected, count
    end
    local function resetLegacyBrokenState()
        local key = legacyBrokenKey()
        owner.legacyBrokenSelection[key] = { 1, 1, 1, 1, 1, 2 }
        if owner.nativeBrokenHornStyle then owner.nativeBrokenHornStyle(1) end
        if owner.nativeBrokenHairColor then owner.nativeBrokenHairColor(0) end
    end
    local categoryDefinitions = {
        { key = "head", label = L("WXL_CC_HEAD", "Head"),
            tooltip = L("WXL_CC_HEAD_TOOLTIP", "Head Customizations") },
        { key = "body", label = L("WXL_CC_BODY", "Body"),
            tooltip = L("WXL_CC_BODY_TOOLTIP", "Body Customizations") },
    }
    local function displayCategory(category)
        if category == "body" or category == "accessories" then return "body" end
        if category == "face" or category == "hair" or category == "facial" or category == "head" then return "head" end
        return category
    end
    local function validCategory(category)
        for _, definition in ipairs(categoryDefinitions) do
            if definition.key == category then return true end
        end
        return false
    end
    if not validCategory(owner.category) then owner.category = "body" end

    function owner:RefreshCategoryAvailability()
        if isTuskarrCreatureFemale() or isMurloc() then
            local available = { body = true }
            self.availableCategories = available
            self.category = "body"
            return available
        end
        if isLegacyBroken() then
            local available = { body = true, head = true }
            self.availableCategories = available
            if not available[self.category] then self.category = "body" end
            return available
        end
        local available = {}
        local count = 0
        local ordinal = 1
        while self.nativeInfo do
            local id, _, _, choices, category = self.nativeInfo(ordinal)
            category = displayCategory(category)
            if not id then break end
            if choices and choices > 1 and validCategory(category) and not available[category] then
                available[category] = true
                count = count + 1
            end
            ordinal = ordinal + 1
        end
        -- Model publication and Glue row layout do not become ready in the same callback. A partial
        -- scan must fail open: hiding navigation at that point makes the later DB2 options
        -- unreachable even though their rows still exist. Once two or more categories have been
        -- observed, the race-specific set is stable enough to narrow the strip.
        if count < 2 then
            available = {}
            for _, definition in ipairs(categoryDefinitions) do
                available[definition.key] = true
            end
        end
        self.availableCategories = available
        if not available[self.category] then
            for _, definition in ipairs(categoryDefinitions) do
                if available[definition.key] then
                    self.category = definition.key
                    break
                end
            end
        end
        return available
    end
    if owner.gearVisible == nil then owner.gearVisible = not CHAR_UNDRESSED end

    local function colorChannels(color)
        color = math.floor(color or 0)
        local red = math.floor(color / 65536) % 256
        local green = math.floor(color / 256) % 256
        local blue = color % 256
        return red / 255, green / 255, blue / 255
    end

    function owner:SetSwatch(texture, color, shown)
        if not shown then
            if texture then texture:Hide() end
            return texture
        end
        if not texture then
            texture = self.swatchOwner:CreateTexture(nil, "OVERLAY")
            texture:SetTexture("Interface\\Custom_Glues\\WXLCustomizationSwatch")
            texture:SetTexCoord(0, 0.65625, 0, 0.625)
            texture:SetWidth(42)
            texture:SetHeight(10)
        end
        local red, green, blue = colorChannels(color)
        texture:SetVertexColor(red, green, blue, 1)
        texture:Show()
        return texture
    end

    function owner:HidePalette()
        if self.palette then self.palette:Hide() end
        self.paletteGlueId = nil
        self.paletteLegacyBroken = nil
    end

    function owner:ComposeStock(glueId)
        local api = C_CharacterCreation
        if not api or type(api.SetCustomizationChoice) ~= "function" then return false end
        local axis = glueId and glueId < self.extraBase and glueId or 1
        self.composeOnly = true
        local forward = pcall(api.SetCustomizationChoice, axis, 1)
        local backward = pcall(api.SetCustomizationChoice, axis, -1)
        self.composeOnly = false
        return forward and backward
    end

    function owner:ApplyPaletteSteps(glueId, delta, steps)
        if steps <= 0 then return end
        -- Keep native arrow choice transitions (including dependent choices), but
        -- submit the synchronous stock composer only for the final selection.
        local cycle = glueId >= self.extraBase and self.nativeCycleOption or self.nativeCycle
        if cycle then
            for _ = 1, steps - 1 do
                if not cycle(glueId, delta) then return end
            end
        else
            for _ = 1, steps - 1 do C_CharacterCreation.SetCustomizationChoice(glueId, delta) end
        end
        C_CharacterCreation.SetCustomizationChoice(glueId, delta)
    end

    function owner:PaletteButton(index)
        local palette = self.palette
        local button = palette.buttons[index]
        if button then return button end

        button = CreateFrame("Button", nil, palette)
        button:SetWidth(24)
        button:SetHeight(24)
        button:SetBackdrop({
            bgFile = "Interface\\Buttons\\WHITE8X8",
            edgeFile = "Interface\\Tooltips\\UI-Tooltip-Border",
            tile = true, tileSize = 8, edgeSize = 9,
            insets = { left = 2, right = 2, top = 2, bottom = 2 },
        })
        button:SetBackdropColor(0.08, 0.08, 0.08, 1)
        button.left = button:CreateTexture(nil, "ARTWORK")
        button.left:SetTexture("Interface\\Buttons\\WHITE8X8")
        button.left:SetPoint("TOPLEFT", button, "TOPLEFT", 4, -4)
        button.left:SetPoint("BOTTOMRIGHT", button, "BOTTOMRIGHT", -4, 4)
        button.right = button:CreateTexture(nil, "OVERLAY")
        button.right:SetTexture("Interface\\Buttons\\WHITE8X8")
        button.right:SetPoint("TOP", button.left, "TOP", 0, 0)
        button.right:SetPoint("BOTTOMRIGHT", button.left, "BOTTOMRIGHT", 0, 0)
        button.right:SetWidth(8)
        button.label = button:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
        button.label:SetPoint("LEFT", button, "LEFT", 6, 0)
        button.label:SetPoint("RIGHT", button, "RIGHT", -6, 0)
        button.label:SetJustifyH("CENTER")
        button:SetScript("OnClick", function(self)
            if owner.paletteLegacyBroken and owner.paletteGlueId and self.choiceOrdinal then
                PlaySound("gsCharacterCreationLook")
                local axis = owner.paletteGlueId
                local selected, count = legacyBrokenState(axis)
                local steps = (self.choiceOrdinal - selected) % count
                for _ = 1, steps do C_CharacterCreation.SetCustomizationChoice(axis, 1) end
                owner:HidePalette()
                owner:RefreshRows()
                return
            end
            if owner.paletteGlueId and self.choiceOrdinal and
                C_CharacterCreation and C_CharacterCreation.SetCustomizationChoice then
                PlaySound("gsCharacterCreationLook")
                local _, _, selected, count = owner.nativeState(owner.paletteGlueId)
                if selected and count and count > 0 then
                    local forward = (self.choiceOrdinal - selected) % count
                    local backward = (selected - self.choiceOrdinal) % count
                    local delta, steps = 1, forward
                    if backward < forward then delta, steps = -1, backward end
                    owner:ApplyPaletteSteps(owner.paletteGlueId, delta, steps)
                end
                owner:HidePalette()
                owner:RefreshRows()
            end
        end)
        button:SetScript("OnEnter", function(self)
            if not GlueTooltip then return end
            GlueTooltip:SetOwner(self, "ANCHOR_RIGHT")
            GlueTooltip:SetText(self.choiceName and self.choiceName ~= "" and self.choiceName or
                ("Option " .. tostring(self.choiceOrdinal or 0)), 1, 1, 1)
            GlueTooltip:Show()
        end)
        button:SetScript("OnLeave", function()
            if GlueTooltip then GlueTooltip:Hide() end
        end)
        palette.buttons[index] = button
        return button
    end

    function owner:EnsurePalette()
        if self.palette then return self.palette end
        local palette = CreateFrame("Frame", nil, GlueParent)
        palette:SetFrameStrata("DIALOG")
        palette:SetBackdrop({
            bgFile = "Interface\\DialogFrame\\UI-DialogBox-Background",
            edgeFile = "Interface\\DialogFrame\\UI-DialogBox-Border",
            tile = true, tileSize = 32, edgeSize = 24,
            insets = { left = 7, right = 7, top = 7, bottom = 7 },
        })
        palette:SetBackdropColor(0.04, 0.04, 0.04, 0.96)
        palette.title = palette:CreateFontString(nil, "OVERLAY", "GameFontNormal")
        palette.title:SetPoint("TOP", palette, "TOP", 0, -10)
        palette.buttons = {}
        palette:Hide()
        self.palette = palette
        return palette
    end

    function owner:TogglePalette(row)
        if not row or not row.CustomizationIndex then return end
        local legacyBroken = isLegacyBroken() and row.CustomizationIndex < self.extraBase
        if not legacyBroken and
            (not self.nativeState or not self.nativeChoice or not self.nativeSetOption) then return end
        if self.palette and self.palette:IsShown() and
            self.paletteGlueId == row.CustomizationIndex then
            self:HidePalette()
            return
        end

        local name, count, hasSwatch
        if legacyBroken then
            name = row.WXLLegacyName or "Customization"
            _, count = legacyBrokenState(row.CustomizationIndex)
            hasSwatch = row.CustomizationIndex == 1 or row.CustomizationIndex == 4
        else
            name, _, _, count, _, _, hasSwatch = self.nativeState(row.CustomizationIndex)
        end
        if not name or not count or count < 1 then return end
        local palette = self:EnsurePalette()
        self.paletteGlueId = row.CustomizationIndex
        self.paletteLegacyBroken = legacyBroken
        palette.title:SetText(name)
        local columns = hasSwatch and 8 or (count <= 8 and 2 or (count <= 24 and 3 or 4))
        local cellWidth = hasSwatch and 30 or 112
        local cellHeight = hasSwatch and 26 or 25
        local stepX = cellWidth + 3
        local stepY = cellHeight + 3
        local rows = math.ceil(count / columns)
        palette:SetWidth(columns * stepX + 18)
        palette:SetHeight(rows * stepY + 40)
        palette:ClearAllPoints()
        palette:SetPoint("TOPRIGHT", row.Background or row, "TOPLEFT", -8, 4)

        for index = 1, count do
            local choiceName, swatch1, swatch2, selected, choiceHasSwatch
            if legacyBroken then
                local current = legacyBrokenState(row.CustomizationIndex)
                choiceName = tostring(index)
                selected = index == current
                choiceHasSwatch = hasSwatch
                if row.CustomizationIndex == 1 then
                    swatch1 = legacyBrokenSkinSwatches[index]
                elseif row.CustomizationIndex == 4 then
                    swatch1 = legacyBrokenHairSwatches[index]
                end
                swatch2 = swatch1
            else
                choiceName, swatch1, swatch2, selected, choiceHasSwatch =
                    self.nativeChoice(row.CustomizationIndex, index)
            end
            local button = self:PaletteButton(index)
            button.choiceOrdinal = index
            button.choiceName = choiceName
            button:SetWidth(cellWidth)
            button:SetHeight(cellHeight)
            button:ClearAllPoints()
            button:SetPoint("TOPLEFT", palette, "TOPLEFT",
                10 + ((index - 1) % columns) * stepX,
                -30 - math.floor((index - 1) / columns) * stepY)
            if hasSwatch and choiceHasSwatch then
                local red, green, blue = colorChannels(swatch1)
                button.left:SetVertexColor(red, green, blue, 1)
                local second = swatch2 and swatch2 ~= 0 and swatch2 ~= swatch1
                button.left:ClearAllPoints()
                button.left:SetPoint("TOPLEFT", button, "TOPLEFT", 4, -4)
                if second then
                    local red2, green2, blue2 = colorChannels(swatch2)
                    button.left:SetPoint("BOTTOMRIGHT", button, "BOTTOM", 0, 4)
                    button.right:SetVertexColor(red2, green2, blue2, 1)
                    button.right:Show()
                else
                    button.left:SetPoint("BOTTOMRIGHT", button, "BOTTOMRIGHT", -4, 4)
                    button.right:Hide()
                end
                button.left:Show()
                button.label:Hide()
            else
                button.left:Hide()
                button.right:Hide()
                button.label:SetText(choiceName and choiceName ~= "" and
                    self:LocalizeChoice(choiceName) or tostring(index))
                button.label:Show()
            end
            button:SetBackdropBorderColor(selected and 1 or 0.35,
                selected and 0.82 or 0.35, selected and 0 or 0.35, 1)
            button:Show()
        end
        for index = count + 1, #palette.buttons do palette.buttons[index]:Hide() end
        palette:Show()
    end

    function owner:Decorate(row)
        if not row or not row.CustomizationIndex or not self.nativeState then return end
        -- Legacy Broken deliberately uses its private ChrRaces/CharSections contract. Decorating a
        -- stock axis with Retail model 33/34 state replaced the real DBC counts (20 faces, several
        -- skin/hair colours) with the Retail carrier's 1-choice face and female hair rows.
        if isLegacyBroken() and row.CustomizationIndex < self.extraBase then
            local axis = row.CustomizationIndex
            local selected, count = legacyBrokenState(axis)
            local name = row.WXLLegacyAxis == axis and row.WXLLegacyName or
                ((row.CustomizationName and row.CustomizationName:GetText()) or "Customization")
            if axis == 5 then name = legacyBrokenKey() == "female" and "Tendrils" or "Horn Style" end
            if legacyBrokenKey() == "female" and axis == 6 then name = "Horn Style" end
            name = self:LocalizeOption(name)
            row.WXLLegacyName = name
            row.WXLLegacyAxis = axis
            if row.CustomizationName then
                self:SetRowText(row, name, selected .. "/" .. count)
            end
            local hasSwatch = axis == 1 or axis == 4
            local swatch = axis == 1 and legacyBrokenSkinSwatches[selected] or
                (axis == 4 and legacyBrokenHairSwatches[selected] or nil)
            self.swatchOwner = row
            row.WXLSwatch1 = self:SetSwatch(row.WXLSwatch1, swatch, hasSwatch)
            row.WXLSwatch2 = self:SetSwatch(row.WXLSwatch2, nil, false)
            self.swatchOwner = nil
            if row.WXLSwatch1 and hasSwatch then
                row.WXLSwatch1:ClearAllPoints()
                row.WXLSwatch1:SetPoint("CENTER", row, "CENTER", 0, 0)
                row.CustomizationName:SetText("")
            end
            if not row.WXLPaletteButton then
                row.WXLPaletteButton = CreateFrame("Button", nil, row)
                row.WXLPaletteButton:SetPoint("TOPLEFT", row.Background or row, "TOPLEFT", 2, -2)
                row.WXLPaletteButton:SetPoint("BOTTOMRIGHT", row.Background or row,
                    "BOTTOMRIGHT", -2, 2)
                row.WXLPaletteButton:SetScript("OnClick", function(self)
                    owner:TogglePalette(self.ownerRow)
                end)
            end
            row.WXLPaletteButton.ownerRow = row
            row.WXLPaletteButton:Show()
            return
        end
        local name, choiceName, selected, count, swatch1, swatch2, hasSwatch =
            self.nativeState(row.CustomizationIndex)
        if not name then
            self:SetRowText(row, row.CustomizationName:GetText() or "Customization", "")
            if row.WXLSwatch1 then row.WXLSwatch1:Hide() end
            if row.WXLSwatch2 then row.WXLSwatch2:Hide() end
            if row.WXLPaletteButton then row.WXLPaletteButton:Hide() end
            return
        end

        local value = choiceName and choiceName ~= "" and self:LocalizeChoice(choiceName) or
            (tostring(selected or 0) .. "/" .. tostring(count or 0))
        if row.CustomizationName then
            self:SetRowText(row, self:LocalizeOption(name), value)
        end

        self.swatchOwner = row
        row.WXLSwatch1 = self:SetSwatch(row.WXLSwatch1, swatch1, hasSwatch)
        local second = hasSwatch and swatch2 and swatch2 ~= 0 and swatch2 ~= swatch1
        row.WXLSwatch2 = self:SetSwatch(row.WXLSwatch2, swatch2, second)
        self.swatchOwner = nil

        if row.WXLSwatch1 and hasSwatch then
            row.WXLSwatch1:ClearAllPoints()
            row.WXLSwatch1:SetPoint("CENTER", row, "CENTER", second and -14 or 0, 0)
            row.CustomizationName:SetText("")
        end
        if row.WXLSwatch2 and second then
            row.WXLSwatch2:ClearAllPoints()
            row.WXLSwatch2:SetPoint("LEFT", row.WXLSwatch1, "RIGHT", 2, 0)
        end

        if not row.WXLPaletteButton then
            row.WXLPaletteButton = CreateFrame("Button", nil, row)
            row.WXLPaletteButton:SetPoint("TOPLEFT", row.Background or row, "TOPLEFT", 2, -2)
            row.WXLPaletteButton:SetPoint("BOTTOMRIGHT", row.Background or row,
                "BOTTOMRIGHT", -2, 2)
            row.WXLPaletteButton:SetScript("OnClick", function(self)
                owner:TogglePalette(self.ownerRow)
            end)
        end
        row.WXLPaletteButton.ownerRow = row
        if count and count > 1 then row.WXLPaletteButton:Show() else row.WXLPaletteButton:Hide() end
    end

    function owner:RestoreCustomizationStand()
        local preview = _G.WXLCharacterCreationPreview
        if preview and type(preview.EnterCustomization) == "function" then
            preview:EnterCustomization()
        elseif _G._WXL_CC_PREVIEW_PLAY_ANIMATION then
            _WXL_CC_PREVIEW_PLAY_ANIMATION(0)
        end
    end

    function owner:GearIsVisible()
        return self.gearVisible ~= false
    end

    function owner:SetGearVisible(frame, visible, quiet)
        visible = visible and true or false
        self.gearVisible = visible
        local setGearVisible = _G._WXL_M2_CUSTOMIZATION_GEAR_VISIBLE
        -- Publish body-layer suppression before ToggleDress can synchronously submit the other outfit.
        if setGearVisible then setGearVisible(visible and 1 or 0) end
        local stockVisible = not CHAR_UNDRESSED
        if stockVisible ~= visible and frame and type(frame.ToggleDress) == "function" then
            frame:ToggleDress()
            -- ToggleDress changes the stock component sources synchronously. On hide, request the
            -- native clear/rebuild once more after that change as well: the first call prevents
            -- equipment paints during the transition, and this one guarantees the uploaded sheet
            -- is composed from the now-naked component state rather than the previous outfit cache.
            if not visible and setGearVisible then setGearVisible(0) end
        end
        -- ToggleDress replaces the stock component sources synchronously, but it does not pulse the
        -- legacy customization composer. Without that pulse the native model slots disappear while
        -- the already-uploaded dressed body sheet remains until another sex/race selection happens.
        -- Compose the unchanged first axis once here so Hide Gear uploads the naked sheet immediately.
        self:ComposeStock(1)
        if not quiet then
            self:RestoreCustomizationStand()
            self.restoreStandDelay = 0.20
            self:UpdateTabs(frame)
        end
    end

    function owner:UpdateTabs(frame)
        local categoryButtons = frame and frame.WXLCategoryButtons or {}
        for name, button in pairs(categoryButtons) do
            if button.Icon then
                button.Icon:SetAtlas("charactercreate-icon-customize-" .. name ..
                    (name == self.category and "-selected" or ""))
            end
            if name == self.category then
                if button.LockHighlight then button:LockHighlight() end
                local text = button.GetFontString and button:GetFontString()
                if text then text:SetTextColor(1, 0.82, 0, 1) end
            else
                if button.UnlockHighlight then button:UnlockHighlight() end
                local text = button.GetFontString and button:GetFontString()
                if text then text:SetTextColor(1, 1, 1, 1) end
            end
        end
        local gear = frame and frame.ToggleUIButton
        local gearVisible = self:GearIsVisible()
        if gear and gear.ButtonTexture then
            gear.ButtonTexture:SetVertexColor(not gearVisible and 1 or 0.72,
                not gearVisible and 0.82 or 0.72, not gearVisible and 0 or 0.72, 1)
        end
        if gear and gear.WXLLabel then
            gear.WXLLabel:SetText(gearVisible and
                L("WXL_CC_HIDE_GEAR", "Hide Gear") or L("WXL_CC_SHOW_GEAR", "Show Gear"))
        end
        if gear and gear.WXLInnerIcon then gear.WXLInnerIcon:Hide() end
    end

    function owner:SetCategory(category)
        if not validCategory(category) then return end
        self:HidePalette()
        self.category = category
        self.page = 1
        local alternate = _G.WXLGilneanPreview
        if alternate and alternate.front then
            alternate.page = 1
            alternate:RefreshOptions()
            self:UpdateTabs(self.wrappedFrame)
            return
        end
        local frame = self.wrappedFrame
        if frame then
            if type(frame.ResetCamera) == "function" then frame:ResetCamera() end
            if category == "head" and C_CharacterCreation and C_CharacterCreation.ZoomCamera then
                -- Earthen occupies a replacement slot whose Glue camera metadata still belongs to
                -- the taller Night Elf Illidari model. A smaller face delta reproduces Dwarf-like
                -- framing instead of pushing the camera above the head.
                local faceZoom = self.selectedClientRace == 24 and 60 or 80
                C_CharacterCreation.ZoomCamera(faceZoom, 0.5, true)
            end
            self:UpdateTabs(frame)
            if type(frame.UpdateCustomizationButtonFrame) == "function" then
                frame:UpdateCustomizationButtonFrame(false)
            end
        end
    end

    function owner:SetButtonTooltip(button, title, detail)
        button.WXLTooltipTitle = title
        button.WXLTooltipDetail = detail
        button:SetScript("OnEnter", function(self)
            if self.ButtonTexture then self.ButtonTexture:SetVertexColor(1, 0.82, 0, 1) end
            if GlueTooltip then
                GlueTooltip:SetOwner(self, "ANCHOR_RIGHT")
                local tooltipTitle = self.WXLTooltipTitle
                if self.WXLCategory == "gear" then
                    tooltipTitle = owner:GearIsVisible() and
                        L("WXL_CC_HIDE_GEAR", "Hide Gear") or L("WXL_CC_SHOW_GEAR", "Show Gear")
                end
                GlueTooltip:SetText(tooltipTitle, 1, 1, 1)
                if self.WXLTooltipDetail then
                    GlueTooltip:AddLine(self.WXLTooltipDetail, 0.8, 0.8, 0.8)
                end
                GlueTooltip:Show()
            end
        end)
        button:SetScript("OnLeave", function(self)
            owner:UpdateTabs(owner.wrappedFrame)
            if GlueTooltip then GlueTooltip:Hide() end
        end)
        button:SetScript("OnUpdate", nil)
    end

    function owner:LabelButton(button, text)
        if not button.WXLLabel then
            button.WXLLabel = button:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
            button.WXLLabel:SetPoint("TOP", button, "BOTTOM", 0, 2)
        end
        button.WXLLabel:SetText(text)
        button.WXLLabel:Show()
    end

    function owner:SetupTabs(frame)
        if not frame or not frame.ToggleUIButton or not frame.SideViewButton or
            not frame.BackViewButton then return end

        frame.SideViewButton:Hide()
        frame.BackViewButton:Hide()
        if frame.SmallButtons and CharacterCreate then
            frame.SmallButtons:ClearAllPoints()
            frame.SmallButtons:SetPoint("TOPLEFT", CharacterCreate, "TOPLEFT", 40, -32)
        end
        frame.WXLCategoryButtons = frame.WXLCategoryButtons or {}
        for key, button in pairs(frame.WXLCategoryButtons) do
            if not validCategory(key) then button:Hide() end
        end
        for i, definition in ipairs(categoryDefinitions) do
            local button = frame.WXLCategoryButtons[definition.key]
            if not button then
                button = CreateFrame("Button", nil, frame)
                button:SetSize(56, 56)
                button.Icon = button:CreateTexture(nil, "ARTWORK")
                button.Icon:SetAllPoints(button)
                button:SetScript("OnClick", function(self)
                    PlaySound("gsCharacterCreationLook")
                    owner:SetCategory(self.WXLCategory)
                end)
                frame.WXLCategoryButtons[definition.key] = button
            end
            button.WXLCategory = definition.key
            button:ClearAllPoints()
            button:SetPoint("TOP", frame, "TOP", (i - 1) * 68 + 38, -65)
            self:LabelButton(button, definition.label)
            self:SetButtonTooltip(button, definition.tooltip)
            button:Show()
        end
        local gear = frame.ToggleUIButton
        gear:ClearAllPoints()
        gear:SetPoint("TOP", frame, "TOP", 174, -65)
        gear.tooltipText = L("WXL_CC_HIDE_GEAR", "Hide Gear")
        gear.uiHidden = false
        gear.WXLCategory = "gear"
        gear:SetAlpha(1)
        gear:Show()
        self:LabelButton(gear, self:GearIsVisible() and
            L("WXL_CC_HIDE_GEAR", "Hide Gear") or L("WXL_CC_SHOW_GEAR", "Show Gear"))
        gear.WXLLabel:ClearAllPoints()
        gear.WXLLabel:SetPoint("TOP", gear, "BOTTOM", 0, 10)
        if gear.WXLInnerIcon then gear.WXLInnerIcon:Hide() end
        self:SetButtonTooltip(gear, L("WXL_CC_HIDE_GEAR", "Hide Gear"),
            L("WXL_CC_GEAR_TOOLTIP", "Toggle equipped preview gear without hiding the controls"))
        gear:SetScript("OnClick", function()
            PlaySound("gsCharacterCreationLook")
            owner:SetGearVisible(frame, not owner:GearIsVisible())
        end)

        if not frame.WXLGearLifecycleHooked and type(frame.HookScript) == "function" then
            frame.WXLGearLifecycleHooked = true
            frame:HookScript("OnHide", function(customizationFrame)
                owner:HidePalette()
                -- Never leak an undressed body atlas into character select or the next recycled CMO.
                if not owner:GearIsVisible() then
                    owner:SetGearVisible(customizationFrame, true, true)
                end
            end)
        end
        self:UpdateTabs(frame)
    end

    function owner:SetRowText(row, name, value)
        if not row.WXLLabel then
            row.WXLLabel = row:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
        end
        row.WXLLabel:SetText(name)
        row.WXLLabel:SetTextColor(1, 1, 1)
        row.WXLLabel:ClearAllPoints()
        row.WXLLabel:SetPoint("RIGHT", row, "LEFT", -32, 0)
        row.WXLLabel:SetWidth(116)
        row.WXLLabel:SetJustifyH("RIGHT")
        row.WXLLabel:Show()
        row.CustomizationName:ClearAllPoints()
        row.CustomizationName:SetPoint("CENTER", row, "CENTER", 0, 0)
        row.CustomizationName:SetWidth(116)
        row.CustomizationName:SetText(value)
        row.CustomizationName:SetTextColor(1, 0.82, 0)
    end

    function owner:StyleRow(row)
        row:SetSize(130, 28)
        if row.Background then
            row.Background:ClearAllPoints()
            row.Background:SetPoint("CENTER", row, "CENTER", 0, -1)
            row.Background:SetSize(142, 46)
        end
        for _, item in ipairs({{row.PrevButton, "RIGHT", "LEFT", -4},
                                {row.NextButton, "LEFT", "RIGHT", 4}}) do
            local button = item[1]
            if button then
                button:ClearAllPoints()
                button:SetPoint(item[2], row, item[3], item[4], 0)
                button:SetSize(26, 26)
                if button.Texture then button.Texture:SetSize(26, 26) end
            end
        end
    end

    function owner:StyleDice(button)
        if not button then return end
        if not button.WXLDice then
            -- Remove the red template artwork, retain its existing randomize callback.
            for _, region in ipairs({button:GetRegions()}) do region:Hide() end
            for _, getter in ipairs({"GetNormalTexture", "GetPushedTexture", "GetHighlightTexture", "GetDisabledTexture"}) do
                local texture = button[getter] and button[getter](button)
                if texture then texture:SetAlpha(0) end
            end
            button.WXLDice = button:CreateTexture(nil, "OVERLAY")
            button.WXLDice:SetAllPoints(button)
            button.WXLDice:SetTexture("Interface\\Custom_Glues\\WXLCustomizationDice")
            button.WXLDice:SetTexCoord(0, 1, 0, 1)
            self:SetButtonTooltip(button, "Randomize Appearance")
        end
        button:SetSize(24, 24)
        button.WXLDice:Show()
    end

    function owner:QueueCustomizationWork(callback)
        if self.customizationWork then return false end
        local api = C_CharacterCreation
        self.customizationWork = { callback = callback, frames = 0, settled = 0,
            race = api.GetSelectedRace(), sex = api.GetSelectedSex() }
        self:ShowModelSpinner(true)
        return true
    end

    function owner:UpdateCustomizationWork()
        local work = self.customizationWork
        if not work then return end
        local root, api = CharacterCreate, C_CharacterCreation
        local panel = root and root.CustomizationFrame
        if not root or not root:IsShown() or not panel or not panel:IsShown() or panel.show == false or
            api.GetSelectedRace() ~= work.race or api.GetSelectedSex() ~= work.sex then
            self.customizationWork = nil
            return
        end
        if work.callback then
            work.frames = work.frames + 1
            if work.frames < 2 then return end -- allow the spinner to render before synchronous work
            local callback = work.callback
            work.callback = nil
            self.runningCustomizationWork = true
            local ok, message = pcall(callback)
            self.runningCustomizationWork = false
            if not ok then
                self.customizationWork = nil
                if geterrorhandler then geterrorhandler()(message) end
            end
            return
        end
        if self.nativePending and self.nativePending() then work.settled = 0
        else work.settled = work.settled + 1 end
        if work.settled >= 2 then self.customizationWork = nil end
    end

    function owner:ShowModelSpinner(loading)
        loading = loading or self.customizationWork ~= nil
        local root = _G.CharacterCreate
        local active = root and root:IsShown()
        if not loading or not active then
            if self.modelSpinner then self.modelSpinner:Hide() end
            return
        end
        local spinner = self.modelSpinner
        if not spinner then
            spinner = CreateFrame("Frame", "WXLModelLoadingSpinner", root, "LoadingSpinnerTemplate")
            self.modelSpinner = spinner
            spinner:SetSize(48, 48)
            spinner:SetPoint("CENTER", root, "CENTER", 0, 0)
            spinner:SetFrameStrata("FULLSCREEN_DIALOG")
            spinner:SetFrameLevel(30)
            spinner:EnableMouse(false)
            spinner:SetScript("OnHide", function(self)
                local anim = self.AnimFrame and self.AnimFrame.Anim
                if anim then anim:Stop() end
            end)
            spinner:Hide()
        end
        if not spinner:IsShown() then
            spinner:Show()
            local anim = spinner.AnimFrame and spinner.AnimFrame.Anim
            if anim then anim:Play() end
        end
    end

    function owner:RefreshModelSpinner()
        local preview = _G.WXLCharacterCreationPreview
        if not preview or type(preview.SetModelLoading) ~= "function" then
            self:ShowModelSpinner(false)
            return
        end
        -- The preview schedules this recovery call before every reveal, even when
        -- all slots are already settled. Avoid another synchronous outfit rebuild.
        if type(preview.nativeRestoreEquipment) == "function" and
            preview.nativeRestoreEquipment ~= self.wrappedEquipmentRecovery then
            local restore = preview.nativeRestoreEquipment
            local wrapper = function(...)
                if preview.IsEquipmentReady and preview:IsEquipmentReady() then return true end
                local start = owner.nativeTiming and owner.nativeTiming()
                local result = restore(...)
                if start then owner.nativeTiming(4, start) end
                return result
            end
            self.wrappedEquipmentRecovery = wrapper
            preview.nativeRestoreEquipment = wrapper
        end
        if preview.SetModelLoading ~= self.wrappedModelLoading then
            local original = preview.SetModelLoading
            local wrapper = function(controller, loading)
                local result = original(controller, loading)
                -- Keep the native model visibility/readiness policy, replace only the old overlay.
                if controller.modelLoading then controller.modelLoading:Hide() end
                owner.modelIsLoading = loading and true or false
                owner:ShowModelSpinner(owner.modelIsLoading)
                return result
            end
            self.wrappedModelLoading = wrapper
            preview.SetModelLoading = wrapper
            -- A model may already be awaiting its first reveal when this bridge is registered.
            self.modelIsLoading = preview.rebuildDelay ~= nil or
                (preview.pending and preview.pending.phase == 1 and not preview.pending.revealedAt) or false
        end
        self:ShowModelSpinner(self.modelIsLoading)
    end

    function owner:RefreshRaceInfo()
        local root = CharacterCreate
        local active = root and root:IsShown() and root.CustomizationFrame and root.CustomizationFrame:IsShown() and root.CustomizationFrame.show ~= false
        if not active then
            if self.raceInfoPanel then self.raceInfoPanel:Hide() end
            return
        end
        local api = C_CharacterCreation
        local selected = api and api.GetSelectedRace and api.GetSelectedRace()
        local info = selected and S_CHARACTER_RACES_INFO and S_CHARACTER_RACES_INFO[selected]
        if not info then
            if self.raceInfoPanel then self.raceInfoPanel:Hide() end
            return
        end
        local sex = api.GetSelectedSex and api.GetSelectedSex()
        local token = (sex == 3 and (info.overrideRaceNameFemale or info.raceNameFemale)) or
            info.overrideRaceName or info.raceName
        local name = token and (_G[token] or token) or info.clientFileString or ""
        local tag = string.upper(info.clientFileString or "")
        local description = _G["CHARACTER_CREATE_INFO_RACE_" .. tag .. "_DESC"] or
            _G["RACE_INFO_" .. tag] or ""
        local factionToken = PLAYER_FACTION_GROUP and PLAYER_FACTION_GROUP[info.factionID]
        local faction = factionToken and (_G[string.upper(factionToken)] or factionToken) or ""
        local key = tostring(selected) .. ":" .. tostring(sex) .. ":" .. name .. ":" .. tostring(root:GetHeight())
        local panel = self.raceInfoPanel
        if not panel then
            panel = CreateFrame("Frame", "WXLCustomizationRaceInfo", root)
            self.raceInfoPanel = panel
            panel:SetPoint("TOPLEFT", root, "TOPLEFT", 36, -94)
            panel:SetSize(208, math.max(300, root:GetHeight() - 180))
            panel:SetFrameStrata("FULLSCREEN_DIALOG")
            panel:SetFrameLevel(20)
            panel.title = panel:CreateFontString(nil, "OVERLAY", "GameFontNormalLarge")
            panel.title:SetPoint("TOPLEFT", panel, "TOPLEFT", 0, 0)
            panel.title:SetWidth(208); panel.title:SetJustifyH("LEFT")
            panel.faction = panel:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
            panel.faction:SetPoint("TOPLEFT", panel, "TOPLEFT", 0, -26)
            panel.description = panel:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
            panel.description:SetPoint("TOPLEFT", panel, "TOPLEFT", 0, -52)
            panel.description:SetWidth(208); panel.description:SetHeight(134)
            panel.description:SetJustifyH("LEFT"); panel.description:SetJustifyV("TOP")
            panel.description:SetTextColor(0.85, 0.85, 0.85)
            panel.descriptionTip = CreateFrame("Button", nil, panel)
            panel.descriptionTip:SetAllPoints(panel.description)
            panel.descriptionTip:SetScript("OnEnter", function()
                if GlueTooltip and panel.fullDescription and panel.fullDescription ~= "" then
                    GlueTooltip:SetOwner(panel.descriptionTip, "ANCHOR_RIGHT")
                    GlueTooltip:SetText(panel.raceName or "", 1, 0.82, 0)
                    GlueTooltip:AddLine(panel.fullDescription, 1, 1, 1, true)
                    GlueTooltip:Show()
                end
            end)
            panel.descriptionTip:SetScript("OnLeave", function() if GlueTooltip then GlueTooltip:Hide() end end)
            panel.heading = panel:CreateFontString(nil, "OVERLAY", "GameFontNormalLarge")
            panel.heading:SetPoint("TOPLEFT", panel, "TOPLEFT", 0, -210)
            panel.rows = {}
            for i = 1, 5 do
                local row = CreateFrame("Button", nil, panel)
                row:SetPoint("TOPLEFT", panel, "TOPLEFT", 0, -245 - (i - 1) * 49)
                row:SetSize(208, 43)
                row.icon = row:CreateTexture(nil, "ARTWORK")
                row.icon:SetSize(32, 32); row.icon:SetPoint("LEFT", row, "LEFT", 0, 0)
                row.text = row:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
                row.text:SetPoint("LEFT", row, "LEFT", 42, 0)
                row.text:SetWidth(166); row.text:SetHeight(42)
                row.text:SetJustifyH("LEFT"); row.text:SetJustifyV("MIDDLE")
                row.text:SetTextColor(0.95, 0.95, 0.95)
                row:SetScript("OnEnter", function(self)
                    if GlueTooltip and self.description then
                        GlueTooltip:SetOwner(self, "ANCHOR_RIGHT")
                        GlueTooltip:SetText(self.description, 1, 1, 1, nil, true)
                        GlueTooltip:Show()
                    end
                end)
                row:SetScript("OnLeave", function() if GlueTooltip then GlueTooltip:Hide() end end)
                panel.rows[i] = row
            end
            panel.pages = CreateFrame("Frame", nil, panel, "CharacterCreateCustomizationButtonFrameTemplate")
            panel.pages:SetSize(100, 26); panel.pages.Background:Hide()
            for _, entry in ipairs({{panel.pages.PrevButton, -1}, {panel.pages.NextButton, 1}}) do
                local delta = entry[2]
                entry[1]:SetScript("OnClick", function()
                    panel.page = math.max(1, math.min(panel.pageCount or 1, (panel.page or 1) + delta))
                    owner.raceInfoKey = nil
                    owner:RefreshRaceInfo()
                end)
            end
        end
        panel:SetHeight(math.max(300, root:GetHeight() - 180))
        panel:SetAlpha(1)
        panel:Show()
        if self.raceInfoKey == key then return end
        self.raceInfoKey = key
        if panel.raceID ~= selected then panel.page = 1 end
        panel.raceID = selected
        panel.raceName = name; panel.fullDescription = description
        panel.title:SetText(name); panel.faction:SetText(faction)
        panel.description:SetText(description)
        local traits = {}
        for i = 1, math.min(TOOLTIP_MAX_RACE_ABLILITIES or 10, 20) do
            local text = _G["CHARACTER_CREATE_INFO_RACE_" .. tag .. "_SPELL" .. i .. "_DESC_SHORT"]
            if text and text ~= "" then
                local icon, detail = string.match(text, "([^|]*)|(.+)")
                traits[#traits + 1] = {icon = icon and ("Interface/Icons/" .. icon), text = detail or text}
            end
        end
        if #traits == 0 then
            for i = 1, 10 do
                local text = _G["RACE_INFO_" .. tag .. i]
                if text and text ~= "" then traits[#traits + 1] = {text = text} end
            end
        end
        panel.heading:SetText(#traits > 0 and "Racial Traits" or "Race Details")
        if #traits == 0 then
            if faction ~= "" then traits[#traits + 1] = {text = faction} end
            local alternate = _G.WXLGilneanPreview
            if alternate and alternate.loaded and alternate.primaryName and alternate.alternateName then
                traits[#traits + 1] = {text = alternate.primaryName .. " / " .. alternate.alternateName .. " forms"}
            end
        end
        local size = math.max(1, math.min(5, math.floor((root:GetHeight() - 430) / 49)))
        panel.pageCount = math.max(1, math.ceil(#traits / size))
        panel.page = math.max(1, math.min(panel.page or 1, panel.pageCount))
        for i, row in ipairs(panel.rows) do
            local trait = i <= size and traits[(panel.page - 1) * size + i]
            if trait then
                row.description = trait.text
                row.text:SetText(trait.text)
                row.text:ClearAllPoints()
                row.text:SetPoint("LEFT", row, "LEFT", trait.icon and 42 or 0, 0)
                row.text:SetWidth(trait.icon and 166 or 208)
                if trait.icon then row.icon:SetTexture(trait.icon); row.icon:Show() else row.icon:Hide() end
                row:Show()
            else row:Hide() end
        end
        panel.pages:ClearAllPoints()
        panel.pages:SetPoint("TOP", panel, "TOP", 0, -248 - size * 49)
        panel.pages.CustomizationName:SetText(panel.page .. " / " .. panel.pageCount)
        if panel.pageCount > 1 then panel.pages:Show() else panel.pages:Hide() end
    end

    function owner:LayoutName()
        local root = CharacterCreate
        local nav = root and root.NavigationFrame
        local edit = nav and nav.CreateNameEditBox
        if not edit then return end
        local active = root:IsShown() and root.CustomizationFrame and root.CustomizationFrame:IsShown() and root.CustomizationFrame.show ~= false
        if active then
            if not self.nameOriginal then
                self.nameOriginal = { width = edit:GetWidth(), points = {} }
                for i = 1, edit:GetNumPoints() do
                    self.nameOriginal.points[i] = {edit:GetPoint(i)}
                end
            end
            edit:ClearAllPoints()
            edit:SetPoint("TOP", root, "TOP", 0, -48)
            edit:SetWidth(240)
            if not self.nameLabel then
                self.nameLabel = edit:CreateFontString(nil, "OVERLAY", "GameFontNormalLarge")
                self.nameLabel:SetPoint("BOTTOM", edit, "TOP", 0, 7)
                self.nameLabel:SetText(NAME or "Name")
            end
            self.nameLabel:Show()
        elseif self.nameOriginal then
            edit:ClearAllPoints()
            for _, point in ipairs(self.nameOriginal.points) do edit:SetPoint(unpack(point)) end
            edit:SetWidth(self.nameOriginal.width)
            self.nameOriginal = nil
            if self.nameLabel then self.nameLabel:Hide() end
        end
    end

    function owner:PageSize(frame)
        return math.max(4, math.min(8, math.floor(((frame:GetHeight() or 540) - 220) / 36)))
    end

    function owner:Layout(frame)
        if not frame or not frame.customizationButtonFramePool then return end
        self:SetupTabs(frame)
        self:LayoutName()
        self:RefreshRaceInfo()
        local alternate = _G.WXLGilneanPreview
        if alternate and alternate.front then alternate:EditorVisibility(); return end
        local rows = {}
        local tuskarrCreatureFemale = isTuskarrCreatureFemale()
        local murloc = isMurloc()
        local murlocFemale = isMurlocFemale()
        for row in frame.customizationButtonFramePool:EnumerateActive() do
            if (not tuskarrCreatureFemale or row.CustomizationIndex == self.extraBase) and
                (not murloc or
                    (not murlocFemale and row.CustomizationIndex == 1) or
                    (murlocFemale and row.CustomizationIndex >= self.extraBase and
                        row.CustomizationIndex < self.extraBase + 2)) then
                rows[#rows + 1] = row
            else row:Hide() end
        end
        table.sort(rows, function(a, b) return (a.CustomizationIndex or 0) < (b.CustomizationIndex or 0) end)
        local size = self:PageSize(frame)
        local pages = math.max(1, math.ceil(#rows / size))
        self.page = math.max(1, math.min(self.page or 1, pages))
        if frame.WXLCustomizationScroll then frame.WXLCustomizationScroll:Hide() end
        if frame.WXLCustomizationScrollBar then frame.WXLCustomizationScrollBar:Hide() end
        for i, row in ipairs(rows) do
            self:StyleRow(row)
            self:Decorate(row)
            if row:GetParent() ~= frame then row:SetParent(frame) end
            row:ClearAllPoints()
            row:SetPoint("TOP", frame, "TOP", 82, -155 - ((i - 1) % size) * 36)
            row:SetAlpha(1)
            if i > (self.page - 1) * size and i <= self.page * size then row:Show() else row:Hide() end
        end
        if not frame.WXLPage then
            local page = CreateFrame("Frame", nil, frame, "CharacterCreateCustomizationButtonFrameTemplate")
            frame.WXLPage = page
            page.Background:Hide()
            page:SetSize(100, 28)
            for _, item in ipairs({{page.PrevButton, -1}, {page.NextButton, 1}}) do
                local delta = item[2]
                item[1]:SetScript("OnClick", function()
                    owner.page = math.max(1, math.min(pages, (owner.page or 1) + delta))
                    owner:HidePalette()
                    owner:Layout(frame)
                end)
            end
        end
        -- Rebind the current page count; races and categories can have different lengths.
        local page = frame.WXLPage
        page.PrevButton:SetScript("OnClick", function() owner.page = math.max(1, owner.page - 1); owner:HidePalette(); owner:Layout(frame) end)
        page.NextButton:SetScript("OnClick", function() owner.page = math.min(pages, owner.page + 1); owner:HidePalette(); owner:Layout(frame) end)
        page:ClearAllPoints()
        page:SetPoint("TOP", frame, "TOP", 69, -163 - size * 36)
        page.CustomizationName:SetText(self.page .. " / " .. pages)
        if pages > 1 then page:Show() else page:Hide() end
        local dice = frame.RandomizeCustomizationButton
        if dice then
            self:StyleDice(dice)
            dice:ClearAllPoints()
            dice:SetPoint("TOP", frame, "TOP", -30, -78)
            dice:SetAlpha(1)
            if murlocFemale then dice:Hide() else dice:Show() end
        end
    end

    function owner:RefreshRows()
        local frame = self.wrappedFrame
        if frame then self:Layout(frame) end
    end

    function owner:WrapFrame()
        local frame = _G.CharacterCreateCustomizationFrame
        if not frame and _G.CharacterCreate then
            frame = _G.CharacterCreate.CustomizationFrame
        end
        if not frame or type(frame.UpdateCustomizationButtonFrame) ~= "function" then return end
        if self.wrappedFrame == frame and
            self.wrappedUpdate == frame.UpdateCustomizationButtonFrame then return end

        local original = frame.UpdateCustomizationButtonFrame
        local wrapped = function(customizationFrame, ...)
            local result = original(customizationFrame, ...)
            owner:Layout(customizationFrame)
            return result
        end
        frame.UpdateCustomizationButtonFrame = wrapped
        self.wrappedFrame = frame
        self.wrappedUpdate = wrapped
    end

    function owner:Wrap()
        local api = _G.C_CharacterCreation
        if not api or type(api.SetCustomizationChoice) ~= "function" or
            type(api.GetAvailableCustomizations) ~= "function" then return end
        if self.wrappedTable == api and self.wrappedChoice == api.SetCustomizationChoice and
            self.wrappedAvailable == api.GetAvailableCustomizations then return end

        local originalChoice = api.SetCustomizationChoice
        local originalAvailable = api.GetAvailableCustomizations
        local wrappedChoice = function(optionID, delta)
            -- Worgen has no modern Hair Color axis. Never submit its stale stock
            -- axis 4 to the legacy compositor: it changes body variation textures.
            if owner.selectedClientRace == 12 and optionID == 4 then return false end
            if owner.composeOnly then return originalChoice(optionID, delta) end
            if isLegacyBroken() and optionID and optionID < owner.extraBase then
                local key = legacyBrokenKey()
                local selected, count = legacyBrokenState(optionID)
                local step = delta and delta > 0 and 1 or -1
                selected = ((selected - 1 + step) % count) + 1
                owner.legacyBrokenSelection[key][optionID] = selected
                if optionID == 4 and owner.nativeBrokenHairColor then
                    owner.nativeBrokenHairColor(selected - 1)
                end
                if optionID == 6 and key == "female" then
                    local result = owner.nativeBrokenHornStyle and owner.nativeBrokenHornStyle(selected - 1)
                    if result then owner:ComposeStock(1) end
                    owner:RefreshRows()
                    return result
                end
                local result = originalChoice(optionID, delta)
                owner:RefreshRows()
                return result
            end
            if optionID and optionID >= owner.extraBase then
                if owner.nativeCycleOption then
                    local result = owner.nativeCycleOption(optionID, delta)
                    -- The native override marks the component dirty, but Glue only consumes that
                    -- dirty state reliably when its stock customization composer is submitted too.
                    -- Legacy-axis rows already get that synchronous submission through
                    -- originalChoice; give independent Retail rows (Eyesight, Eye Style, etc.) the
                    -- same pulse so their related-choice materials and geosets update immediately.
                    if result and not isMurlocFemale() then owner:ComposeStock(optionID) end
                    owner:RefreshRows()
                    return result
                end
                return false
            end
            -- Publish the modern override first. The stock call below is the synchronous composer
            -- trigger; doing this afterwards made its first rebuild use the previous modern choice.
            if owner.nativeCycle then owner.nativeCycle(optionID, delta) end
            local result = originalChoice(optionID, delta)
            owner:RefreshRows()
            return result
        end
        api.SetCustomizationChoice = wrappedChoice

        local wrappedAvailable = function(...)
            local stock = originalAvailable(...) or {}
            local styles = {}
            local stockAxis = {}
            local stockCategoryByAxis = {}
            local detectedCategories = {}
            local detectedCount = 0
            local tuskarrCreatureFemale = isTuskarrCreatureFemale()
            local murloc = isMurloc()
            local murlocFemale = isMurlocFemale()
            local legacyBroken = isLegacyBroken()
            for _, style in ipairs(stock) do
                if style and style.orderIndex then stockAxis[style.orderIndex] = true end
                local axis = style and style.orderIndex
                local stockCategory = axis == 1 and "body" or
                    (axis == 2 and "face" or
                    ((axis == 3 or axis == 4) and "hair" or "facial"))
                if axis then stockCategoryByAxis[axis] = stockCategory end
                if legacyBroken and style then
                    if axis == 1 then style.name = "Skin Color" end
                    if axis == 4 then style.name = "Hair Color" end
                    if axis == 5 and legacyBrokenKey() == "male" then style.name = "Horn Style" end
                end
                -- The private legacy Broken rows still inherit several Retail-era labels from the
                -- shared Glue table. None of these four axes has a backing legacy model/material
                -- choice, so presenting them only cycles inert state (for both sexes).
                local brokenDead = legacyBroken and style and style.name and
                    (style.name == "Tattoo" or string.find(style.name, "Eye", 1, true))
                local brokenMeaningful = not legacyBroken or not brokenDead
                if legacyBroken and legacyBrokenKey() == "female" and axis == 5 then
                    -- Verified against the private female model: this axis selects hanging
                    -- facial tendrils (101..105), not the separate forehead/horn group.
                    style.name = "Tendrils"
                end
                if style and style.name then style.name = owner:LocalizeOption(style.name) end
                if legacyBroken and brokenMeaningful then
                    detectedCategories[displayCategory(stockCategory)] = true
                end
                local murlocMeaningful = not murloc or (not murlocFemale and axis == 1)
                local worgenMeaningful = owner.selectedClientRace ~= 12 or axis ~= 4
                if not tuskarrCreatureFemale and brokenMeaningful and murlocMeaningful and worgenMeaningful and
                    owner.category == displayCategory(stockCategory) then
                    styles[#styles + 1] = style
                end
            end
            local mappedLegacyAxis = {}
            local ordinal = 1
            while owner.nativeInfo do
                local id, name, legacyAxis, choices, category = owner.nativeInfo(ordinal)
                if not id then break end
                -- Do not mix Retail Broken model 33/34 choices into the deliberately restored old
                -- models. Their actual five axes are already represented by the stock DBC rows.
                local meaningful = not legacyBroken and (not murloc or murlocFemale) and
                    (not tuskarrCreatureFemale or name == "Skin Color")
                if meaningful and choices and choices > 1 and validCategory(displayCategory(category)) and
                    not detectedCategories[displayCategory(category)] then
                    detectedCategories[displayCategory(category)] = true
                    detectedCount = detectedCount + 1
                end
                local categoryMatches = owner.category == displayCategory(category)
                if meaningful and choices and choices > 1 and categoryMatches then
                    -- The first modern analogue of each legacy axis is already controlled by the
                    -- matching stock row. Later options with the same broad label remain independent.
                    if not tuskarrCreatureFemale and legacyAxis and legacyAxis > 0 and
                        stockAxis[legacyAxis] and stockCategoryByAxis[legacyAxis] == category and
                        not mappedLegacyAxis[legacyAxis] then
                        mappedLegacyAxis[legacyAxis] = true
                    else
                        styles[#styles + 1] = {
                            orderIndex = id, name = owner:LocalizeOption(name)
                        }
                    end
                end
                ordinal = ordinal + 1
            end
            if legacyBroken and legacyBrokenKey() == "female" and owner.category == "head" then
                styles[#styles + 1] = {
                    orderIndex = 6,
                    name = owner:LocalizeOption("Horn Style")
                }
            end
            if legacyBroken then
                detectedCategories = { body = true, head = true }
            elseif murloc then
                detectedCategories = { body = true }
            elseif detectedCount < 2 and not tuskarrCreatureFemale then
                detectedCategories = {}
                for _, definition in ipairs(categoryDefinitions) do
                    detectedCategories[definition.key] = true
                end
            end
            owner.availableCategories = detectedCategories
            return styles
        end
        api.GetAvailableCustomizations = wrappedAvailable

        -- Glue's race enum is selection order, not the private ChrRaces.dbc id used by native model
        -- and appearance code. Passing the ordinal directly makes, for example, selection 11
        -- (Earthen) briefly select client race 11 (Draenei) until the M2 path is observed. Publish
        -- the explicit client id on the selection event so the first composed sheet is already for
        -- the right race. A late Glue identity layer may provide dbcRaceID for replacement slots;
        -- the full table keeps this bridge independent from that optional Lua payload.
        local function wrapReset(name, native, publishContext)
            local original = api[name]
            if type(original) ~= "function" then return end
            api[name] = function(...)
                if name == "RandomizeCharCustomization" and not owner.runningCustomizationWork and
                    CharacterCreate and CharacterCreate:IsShown() and CharacterCreate.CustomizationFrame and
                    CharacterCreate.CustomizationFrame:IsShown() and CharacterCreate.CustomizationFrame.show ~= false then
                    local args = {...}
                    return owner:QueueCustomizationWork(function() api[name](unpack(args)) end)
                end
                if name == "RandomizeCharCustomization" and isTuskarrCreatureFemale() then
                    -- This creature preview has no stock female humanoid axes.
                    -- Use only the verified skin variants, without stock randomize
                    -- or ComposeStock (both enter the legacy humanoid compositor).
                    local result = native and native()
                    owner:RefreshRows()
                    return result
                end
                if name == "RandomizeCharCustomization" and isLegacyBroken() then
                    for axis = 1, (legacyBrokenKey() == "female" and 6 or 5) do
                        local selected, count = legacyBrokenState(axis)
                        local target = math.random(1, count)
                        local steps = (target - selected) % count
                        for _ = 1, steps do api.SetCustomizationChoice(axis, 1) end
                    end
                    owner:RefreshRows()
                    return true
                end
                local measure = publishContext and owner.nativeTiming
                local started = measure and measure()
                local result = original(...)
                if measure then started = measure(1, started) end
                if native then
                    if publishContext then
                        owner.selectedClientRace = selectedClientRace()
                        native(owner.selectedClientRace, api.GetSelectedSex())
                    else
                        native()
                    end
                end
                if measure then started = measure(2, started) end
                if isLegacyBroken() then resetLegacyBrokenState() end
                if native == owner.nativeRandomize then owner:ComposeStock(1) end
                if name == "SetSelectedSex" then
                    -- The class-preview wrapper treats a sex rebuild like a fresh showcase and
                    -- starts its combat sequence. We are already inside customization: cancel that
                    -- pending showcase after the model swap and settle Stand once more after Glue.
                    local preview = _G.WXLCharacterCreationPreview
                    if preview and type(preview.EnterCustomization) == "function" then
                        preview:EnterCustomization()
                    else
                        owner:RestoreCustomizationStand()
                    end
                    owner.restoreStandDelay = 0.20
                end
                owner:RefreshRows()
                if measure then measure(3, started) end
                return result
            end
        end
        wrapReset("ResetCharCustomize", owner.nativeReset)
        wrapReset("RandomizeCharCustomization", owner.nativeRandomize)
        wrapReset("SetSelectedRace", owner.nativeReset, true)
        wrapReset("SetSelectedSex", owner.nativeReset, true)

        self.wrappedTable = api
        self.wrappedChoice = wrappedChoice
        self.wrappedAvailable = wrappedAvailable
    end

    -- At Glue startup the stock character-creation singleton does not exist yet.  Generation zero
    -- means CustomizeNoteModel has not observed a real preview model, so remember it without asking
    -- the stock frame to enumerate customizations.  Calling that path during the white-screen
    -- transition dereferences the client's null character-creation global.
    owner.generation = owner.nativeGeneration and owner.nativeGeneration() or 0

    if not owner.frame then
        owner.frame = CreateFrame("Frame")
        owner.frame:SetScript("OnUpdate", function(_, elapsed)
            owner:UpdateCustomizationWork()
            owner:RefreshModelSpinner()
            -- Resolve the independent form control first. The broader customization wrapper can
            -- legitimately be mid-rebuild while Glue changes race/sex; it must not starve this UI.
            owner.dualFormRefreshElapsed = (owner.dualFormRefreshElapsed or 0) +
                (elapsed or 0)
            if owner.dualFormRefreshElapsed >= 0.10 then
                owner.dualFormRefreshElapsed = 0
                owner:RefreshDualFormPreview(false)
            end
            local preview = _G.WXLCharacterCreationPreview
            local classID = preview and preview.classID or _G.SELECTED_CLASS
            if owner.nativeClass and classID and classID ~= owner.classID then
                owner.classID = classID
                if owner.nativeClass(classID) then
                    -- Class-gated eye shells (notably DK 1701) are outside the five stock bytes.
                    -- Submit one harmless stock composition after publishing the class so the
                    -- already-created preview consumes the new geoset decision immediately.
                    owner:ComposeStock(1)
                end
            end
            owner:Wrap()
            owner:WrapFrame()
            owner:LayoutName()
            owner:RefreshRaceInfo()
            local customizationFrame = owner.wrappedFrame
            if owner.palette and owner.palette:IsShown() and customizationFrame and
                not customizationFrame:IsShown() then
                owner:HidePalette()
            end
            if owner.restoreStandDelay then
                owner.restoreStandDelay = owner.restoreStandDelay - (elapsed or 0)
                if owner.restoreStandDelay <= 0 then
                    owner.restoreStandDelay = nil
                    local frame = owner.wrappedFrame
                    if frame and frame:IsShown() then owner:RestoreCustomizationStand() end
                end
            end
            if owner.nativeGeneration then
                local generation = owner.nativeGeneration()
                local frame = owner.wrappedFrame
                if generation and generation > 0 and generation ~= owner.generation and
                    frame and frame:IsShown() and
                    type(frame.UpdateCustomizationButtonFrame) == "function" then
                    -- Set this before entering the stock update: it can run scripts which yield a
                    -- nested frame update, and the refresh must remain one-shot under re-entry.
                    owner.generation = generation
                    if frame then
                        frame:UpdateCustomizationButtonFrame(false)
                    end
                end
            end
        end)
    end
end
)lua";
}

namespace wxl_modern_m2
{
    bool InstallCharacterCustomize()
    {
        WLOG_INFO("customization-owner-v1: preview-only UI context; per-CMO recipe ownership");
        Forget();
        g_api->UiAddPanel("Modern race customization", &DrawPanel, nullptr);

        const WXL_FrameScriptApi* framescript = FrameScript();
        if (!framescript)
        {
            WLOG_WARN("customization-bridge: FrameScript service unavailable; five Glue buttons stay legacy-only");
            return true;
        }
        const bool bridge =
            framescript->RegisterFunction("_WXL_M2_CUSTOMIZATION_CYCLE", &LuaCycleAxis) != 0 &&
            framescript->RegisterFunction("_WXL_M2_CUSTOMIZATION_OPTION", &LuaModernOptionInfo) != 0 &&
            framescript->RegisterFunction("_WXL_M2_CUSTOMIZATION_STATE",
                                          &LuaModernOptionState) != 0 &&
            framescript->RegisterFunction("_WXL_M2_CUSTOMIZATION_CHOICE",
                                          &LuaModernChoiceInfo) != 0 &&
            framescript->RegisterFunction("_WXL_M2_CUSTOMIZATION_OPTION_SET",
                                          &LuaSetModernOption) != 0 &&
            framescript->RegisterFunction("_WXL_M2_CUSTOMIZATION_OPTION_CYCLE",
                                          &LuaCycleModernOption) != 0 &&
            framescript->RegisterFunction("_WXL_M2_CUSTOMIZATION_GENERATION",
                                          &LuaCustomizationGeneration) != 0 &&
            framescript->RegisterFunction("_WXL_M2_CUSTOMIZATION_PENDING", &LuaCustomizationPending) != 0 &&
            framescript->RegisterFunction("_WXL_M2_CUSTOMIZATION_TIMING", &LuaCustomizationTiming) != 0 &&
            framescript->RegisterFunction("_WXL_M2_CUSTOMIZATION_IDENTITY",
                                          &LuaCustomizationIdentity) != 0 &&
            framescript->RegisterFunction("_WXL_M2_CUSTOMIZATION_BROKEN_HAIR_COLOR",
                                          &LuaLegacyBrokenHairColor) != 0 &&
            framescript->RegisterFunction("_WXL_M2_CUSTOMIZATION_BROKEN_HORN_STYLE",
                                          &LuaLegacyBrokenHornStyle) != 0 &&
            framescript->RegisterFunction("_WXL_M2_CUSTOMIZATION_TUSKARR_CREATURE_FEMALE",
                                          &LuaIsTuskarrCreatureFemale) != 0 &&
            framescript->RegisterFunction("_WXL_M2_CUSTOMIZATION_CLASS",
                                          &LuaSelectedClass) != 0 &&
            framescript->RegisterFunction("_WXL_M2_DUAL_FORM_INFO",
                                          &LuaDualFormInfo) != 0 &&
            framescript->RegisterFunction("_WXL_M2_CUSTOMIZATION_RESET", &LuaResetAxes) != 0 &&
            framescript->RegisterFunction("_WXL_M2_CUSTOMIZATION_RANDOMIZE",
                                          &LuaRandomizeAxes) != 0 &&
            framescript->RegisterScript("wxl-modern-m2-customization", kGlueCustomizationBridge) != 0;
        if (bridge)
            WLOG_INFO("customization-bridge: registered five legacy axes plus DB2 modern option rows");
        else
            WLOG_WARN("customization-bridge: one or more Glue surfaces failed to register");
        return true;
    }
}
