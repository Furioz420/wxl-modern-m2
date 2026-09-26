// Character geoset decision for models carrying a modern geoset numbering.
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
//   The per-instance visibility array is allocated all-visible, and the only thing that ever clears an
//   entry is a hide over an id RANGE. The stock character decision opens with one such hide covering
//   0..2000 and then re-shows what it chose. Both halves of that are sized for a body whose ids stop
//   below 1900.
//
//   A modern race model numbers its submeshes well past that -- feet, tusks, brows and jewellery sit in
//   groups the stock vocabulary has no name for. None of them is ever hidden, so every variant of every
//   one of those groups draws at once, permanently, stacked on top of itself.
//
//   This runs after the stock decision. Past the ceiling it clears the whole band and then puts one
//   variant back per group, decided by the tables where the tables claim the group and by the model's
//   own numbering where they do not. Below the ceiling only the ids the tables claim are re-decided;
//   the equipment the stock decision read off what is actually worn is left alone, since re-deciding
//   that here would be a second opinion with less information.
//
//   Which pieces to stand on comes from the tables, per race and per sex, and falls back to one value
//   per group only for a race the tables cannot name at all.
//
//   The last move matters as much as the first two: the draw list is a compacted copy of the
//   visibility array, so a change that is not followed by a rebuild is a change nothing reads.

#include "DragonHornPolicy.hpp"
#include "ModernM2.hpp"

#include "ExtensionApi.hpp"
#include "GilneanPreview.hpp"
#include "game/World.hpp"
#include "wxl/AppearanceProtocol.hpp"

#include "engine/assets/shared/common/Text.hpp"
#include "engine/assets/shared/models/m2/M2Format.hpp"
#include "game/Binding.hpp"
#include "game/M2.hpp"
#include "offsets/game/M2.hpp"
#include "wxl/AppearanceApi.h"
#include "wxl/FdidApi.h"

#include <cstdint>
#include <cstdio>

namespace
{
    namespace off  = wxl::offsets::game::m2;
    namespace text = wxl::modern::assets::common::text;

    off::Char_GeosetRenderPrepFn g_origGeosetRenderPrep = nullptr;
    off::M2_SetGeometryVisibleFn g_origSetGeometryVisible = nullptr;

    /**
     * @brief Names a section by its geoset alone, so a skin that carries a 32-bit index start can
     *        still be addressed by geoset.
     *
     * The stock scan matches on a full dword read at the submesh's offset 0, which is the geoset id
     * and `level` side by side. That is harmless while level is the LOD marker it was designed to be,
     * and fatal once level carries the high half of an index start: every section past the 16-bit line
     * answers to `65536 + id`, matches no range anyone will ever ask for, and so can never be hidden.
     * It draws forever, next to every other variant of its own group -- which is what a modern race
     * model looks like on this client.
     *
     * The scan is otherwise reproduced exactly: same order, same "did anything actually flip" test,
     * same single rebuild-request at the end. Only the id is read at its own width.
     */
    void __fastcall hkSetGeometryVisible(void* instance, void* edx, uint32_t idStart, uint32_t idEnd,
                                         uint32_t visible)
    {
        void* const model = instance
            ? *reinterpret_cast<void**>(static_cast<uint8_t*>(instance) + off::kOffInstShared)
            : nullptr;
        // Before the instance is live the call is a deferred command, and the replay comes back
        // through here once it is; there is nothing to scan yet either way.
        const bool live = instance &&
            (*reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(instance) + off::kOffInstInitFlags) & 1);
        if (!live || !model || !wxl::modern::assets::m2::IsNativeLoaded(model))
        {
            g_origSetGeometryVisible(instance, edx, idStart, idEnd, visible);
            return;
        }

        const auto* skin = wxl::game::m2::M2Model(model).GetSkin();
        auto* const shown =
            *reinterpret_cast<uint32_t**>(static_cast<uint8_t*>(instance) + off::kOffInstSectionVisible);
        if (!skin || !skin->submeshes || !shown)
        {
            g_origSetGeometryVisible(instance, edx, idStart, idEnd, visible);
            return;
        }

        bool flipped = false;
        for (uint32_t i = 0; i < skin->submeshCount; ++i)
        {
            const uint32_t id = skin->submeshes[i].skinSectionId;
            if (id < idStart || id > idEnd) continue;
            if ((shown[i] == 0) == (visible == 0)) continue;
            shown[i] = visible;
            flipped = true;
        }
        if (flipped)
            wxl::game::Native<off::M2_UnoptimizeVisibleGeometryFn>(off::kUnoptimizeVisibleGeometry)(
                instance, nullptr);
    }

    /// Highest id the stock decision ever names. Its opening hide ends here, and every id it shows
    /// afterwards -- its own slots and the bands it reads off the equipped items -- is below it, so
    /// everything above is territory the stock decision has no opinion about whatsoever.
    constexpr uint32_t kStockIdCeiling = 2000;
    /// One past the highest id a shipped modern race model has been measured to carry.
    constexpr uint32_t kModernIdCeiling = 6000;
    /// Ids are `group * 100 + value`, which is what makes "one value per group" expressible at all.
    constexpr uint32_t kGroupStride = 100;
    /// The band walked group by group, which is the only way to give a group exactly one variant when
    /// no recipe names it: membership in what the tables claim decides who chooses, not a threshold.
    constexpr uint32_t kFirstModernGroup = kStockIdCeiling / kGroupStride;
    constexpr uint32_t kLastModernGroup  = kModernIdCeiling / kGroupStride;

    /// Reads back the ids the finished decision left standing; the panel steps through this.
    void CollectShown(void* instance, const wxl::game::m2::M2SkinProfile* skin);

    // --- the tables' own answer to "which pieces" ------------------------------------------------
    //
    // Showing the lowest id of each group was a placeholder that had to be arbitrary: it reads the
    // model's numbering and nothing else, so it cannot know that this face goes with those tusks, and
    // it gets a different kind of wrong on every race. The tables know, and they know per race.
    //
    // What is still missing is which choices a GIVEN character made: the stock client keeps five
    // legacy bytes, and the table that used to map them onto modern choices ships zero rows. So the
    // first choice of every option stands in for now -- a real appearance out of the tables rather
    // than the character's own. Replacing that is a separate piece of work, and everything below is
    // already shaped for it.

    const WXL_AppearanceApi* g_appearance = nullptr;

    const WXL_FdidApi* g_fdid = nullptr;

    const WXL_FdidApi* Files()
    {
        if (!g_fdid)
            g_fdid = static_cast<const WXL_FdidApi*>(
                wxl_modern_m2::g_api->GetInterface("wxl.fdid", WXL_FDID_API_VERSION));
        return g_fdid;
    }

    /**
     * @brief Names the layout's regions and the layers one choice paints into them, once per model.
     *
     * The half of the sheet the stock arrangement does not reach is filled from these, and nothing
     * about their geometry is knowable from this side: a layer states a material and a mask of
     * sections, and whether its texture is one section's worth or the whole sheet's decides how it
     * has to be copied. Both are read off the tables here rather than assumed, because assuming the
     * scale of a source is exactly what put the face band under the legs earlier.
     */
    void ReportLayout(const WXL_AppearanceApi* tables, uint32_t layoutId, uint32_t chrModel)
    {
        static uint32_t reported = 0;
        if (reported == chrModel || !tables) return;
        reported = chrModel;

        uint32_t width = 0, height = 0;
        tables->LayoutSize(layoutId, &width, &height);
        const uint32_t sections = tables->SectionCount(layoutId);
        WLOG_INFO("char-layers: layout %u is %ux%u in %u section(s)", layoutId, width, height,
                  sections);

        for (uint32_t i = 0; i < sections; ++i)
        {
            WXL_TextureSection section{};
            if (!tables->SectionAt(layoutId, i, &section)) continue;
            WLOG_INFO("char-layers:   section type %2u at (%u,%u) %ux%u, overlaps 0x%X",
                      section.sectionType, section.x, section.y, section.width, section.height,
                      section.overlapMask);
        }
    }

    /// The layers one taken choice contributes, with the file each names. Bounded: a handful of
    /// choices carrying a handful of layers each is the whole shape of the question.
    void ReportLayers(const WXL_Recipe& recipe, uint32_t choiceId)
    {
        static uint32_t left = 24;
        const WXL_FdidApi* files = Files();

        for (uint32_t i = 0; i < recipe.layerCount && left; ++i)
        {
            const WXL_TextureLayer& layer = recipe.layers[i];
            --left;
            const char* path = files
                ? files->ResolveMaterialTexture(layer.materialResourceId, layer.textureType)
                : nullptr;
            WLOG_INFO("char-layers: choice %u layer %u: texture type %u, order %u, blend %u,"
                      " sections 0x%08X, material %u -> %s",
                      choiceId, i, layer.textureType, layer.layer, layer.blendMode,
                      layer.sectionMask, layer.materialResourceId, path ? path : "(unresolved)");
        }
    }

    const WXL_AppearanceApi* Appearance()
    {
        if (!g_appearance)
            g_appearance = static_cast<const WXL_AppearanceApi*>(
                wxl_modern_m2::g_api->GetInterface("wxl.appearance", WXL_APPEARANCE_API_VERSION));
        return g_appearance;
    }

    /// The playable races this client can display, in the order their model paths spell them.
    struct RaceStem { const char* stem; uint32_t raceId; uint32_t sex; };
    constexpr RaceStem kRaceStems[] = {
        // Put compound allied-race directory names before their parent suffixes. For example,
        // "darkirondwarf\\male" also contains "dwarf\\male" and
        // "zandalaritroll\\male" also contains "troll\\male".
        { "vulpera\\male", 9, 0 }, { "vulpera\\female", 9, 1 },
        { "nightborne\\male", 13, 0 }, { "nightborne\\female", 13, 1 },
        { "voidelf\\male", 15, 0 }, { "voidelf\\female", 15, 1 },
        { "zandalaritroll\\male", 18, 0 }, { "zandalaritroll\\female", 18, 1 },
        { "zandalari\\male", 18, 0 }, { "zandalari\\female", 18, 1 },
        { "lightforgeddraenei\\male", 20, 0 }, { "lightforgeddraenei\\female", 20, 1 },
        { "lightforged\\male", 20, 0 }, { "lightforged\\female", 20, 1 },
        { "darkirondwarf\\male", 29, 0 }, { "darkirondwarf\\female", 29, 1 },
        // The private ChrRaces rows request these models through character\\naga_ while HdSwitch
        // redirects the bytes to Kul Tiran. Match the distinctive filename as well as the Retail
        // directory, before the generic Naga aliases below, or the body receives race 13's recipe.
        { "kultiran\\male", 31, 0 }, { "kultiran\\female", 31, 1 },
        { "kultiranmale", 31, 0 }, { "kultiranfemale", 31, 1 },
        { "earthendwarf\\male", 24, 0 }, { "earthendwarf\\female", 24, 1 },
        { "earthendwarfmale", 24, 0 }, { "earthendwarffemale", 24, 1 },
        { "harronir\\male", 27, 0 }, { "harronir\\female", 27, 1 },
        { "harronirmale", 27, 0 }, { "harronirfemale", 27, 1 },
        // Retail uses one ChrModel/path for both Dracthyr factions and sexes. The path therefore
        // cannot recover either distinction; client race 17 / sex 0 is the canonical route to that
        // shared model, option set, layout and geosets.
        { "dracthyrdragon", 17, 0 },
        { "dracthyr\\male", 17, 0 }, { "dracthyr\\female", 17, 1 },
        { "dracthyrmale", 17, 0 }, { "dracthyrfemale", 17, 1 },
        { "worgen\\male", 12, 0 }, { "worgen\\female", 12, 1 },
        { "goblin\\male", 21, 0 }, { "goblin\\female", 21, 1 },
        // Both faction rows use the same Pandaren ChrModel pair. Path alone cannot name faction;
        // the Alliance mapping is sufficient for model, options, layout and geosets.
        { "pandaren\\male", 26, 0 }, { "pandaren\\female", 26, 1 },
        { "eredar\\male", 16, 0 }, { "eredar\\female", 16, 1 },
        // Private client ids are translated by RetailCharacterRace; path recognition must retain
        // the client identity so Tuskarr/Broken/Naga reach their verified Retail recipes and the
        // unsupported Ogre/Murloc families still fail closed.
        { "tuskarrmale", 14, 0 }, { "tuskarrfemale", 14, 1 },
        { "ogre\\male", 19, 0 }, { "ogre\\female", 19, 1 },
        { "broken\\male", 23, 0 }, { "broken\\female", 23, 1 },
        { "naga_\\male", 25, 0 }, { "naga_\\female", 25, 1 },
        { "naga_male", 25, 0 }, { "naga_female", 25, 1 },
        { "murloc2", 28, 0 }, { "murloc\\male", 28, 0 }, { "murloc\\female", 28, 1 },
        // The private Earthen/Harronir rows still enter through the former Illidari paths. Storage
        // redirects the bytes and companions to the Retail family, but the model object's cache key
        // intentionally remains the path the Glue requested. Preserve that identity bridge here too,
        // otherwise the redirected model receives the one-per-group fallback instead of its recipe.
        { "nightelf_dh\\male", 24, 0 }, { "nightelf_dh\\female", 24, 1 },
        { "bloodelf_dh\\male", 27, 0 }, { "bloodelf_dh\\female", 27, 1 },
        { "human\\male",     1, 0 }, { "human\\female",     1, 1 },
        { "orc\\male",       2, 0 }, { "orc\\female",       2, 1 },
        { "dwarf\\male",     3, 0 }, { "dwarf\\female",     3, 1 },
        { "nightelf\\male",  4, 0 }, { "nightelf\\female",  4, 1 },
        { "scourge\\male",   5, 0 }, { "scourge\\female",   5, 1 },
        { "tauren\\male",    6, 0 }, { "tauren\\female",    6, 1 },
        { "gnome\\male",     7, 0 }, { "gnome\\female",     7, 1 },
        { "troll\\male",     8, 0 }, { "troll\\female",     8, 1 },
        { "bloodelf\\male", 10, 0 }, { "bloodelf\\female", 10, 1 },
        { "draenei\\male",  11, 0 }, { "draenei\\female",  11, 1 },
    };

    /// Recovers race and sex from the model path, because the character component's own fields are
    /// not established yet and the path already says it unambiguously for every race that can appear.
    bool RaceOf(const char* stem, uint32_t& raceId, uint32_t& sex)
    {
        if (!stem) return false;
        for (const RaceStem& entry : kRaceStems)
            if (text::ContainsCI(stem, entry.stem))
            {
                raceId = entry.raceId;
                sex = entry.sex;
                // Both Dracthyr factions share this path/model. Keep the selected
                // client faction or every visage rebuild resets its choices.
                uint32_t selectedRace = 0, selectedSex = 0;
                if (raceId == 17 && wxl_modern_m2::CustomizeIdentity(selectedRace, selectedSex) &&
                    (selectedRace == 17 || selectedRace == 30) && selectedSex == sex)
                    raceId = selectedRace;
                return true;
            }
        return false;
    }

    /// Every choice of every option of one model, for the pass that asks what the tables can name at
    /// all. Sized past the widest shipped race rather than to a guess; a model that overflowed it
    /// would under-report what it owns, so the fill says when it is full.
    constexpr uint32_t kMaxChoices = 1024;

    /// One settled choice per option, and a model offers a couple of dozen.
    constexpr uint32_t kMaxSettled = 64;
    /// Distinct geoset ids one model's customization can produce.
    constexpr uint32_t kMaxOwned = 256;
    /// One selected row per ChrCustomizationSkinnedModel element.
    constexpr uint32_t kMaxAttached = 256;

    /**
     * @brief What the tables say about one model: every geoset id its customization can ever produce,
     *        and the subset this appearance stands on.
     *
     * The two are needed together. Knowing only what to show cannot say what to hide, and hiding by
     * number was the thing that could not be made right: the group vocabulary is per race and per sex,
     * so a threshold that holds for one model names somebody else's ears on the next. Membership in
     * `owned` replaces it -- a geoset belongs to the customization exactly when the tables name it
     * here, and everything they do not name is the stock decision's equipment and stays untouched.
     */
    struct Selection
    {
        uint16_t owned[kMaxOwned];
        uint32_t ownedCount = 0;
        uint16_t chosen[kMaxOwned];
        uint32_t chosenCount = 0;
        uint32_t modelFileDataId = 0;
        uint32_t chrModel = 0;
        uint32_t recipeGeneration = 0;
        uint32_t textureLayoutId = 0;
        uint32_t layerCount = 0;
        uint32_t optionCount = 0;
        /// One settled choice per option, which is what BOTH the geometry and the sheet stand on.
        uint32_t choices[kMaxSettled];
        uint32_t choiceCount = 0;
        bool     choicesTruncated = false;
        /// A selected eye material needs a real eye section even when the choice only names texture.
        bool     hasEyeTexture = false;
        wxl_modern_m2::CustomizationAttachmentSpec attached[kMaxAttached];
        uint32_t attachedCount = 0;
        bool     attachmentsTruncated = false;
    };

    /**
     * @brief The choices one model settled on, kept so the sheet can stand on the same ones.
     *
     * Written when the geometry is decided, which the client always does first: it asks for the model
     * before it composes anything onto it. A model absent from here has not been decided yet, and the
     * sheet says so rather than deciding a second time and disagreeing.
     */
    struct SettledChoices
    {
        void* component;
        void* root;
        uint32_t chrModel;
        uint32_t count;
        uint32_t ids[kMaxSettled];
    };
    SettledChoices g_settled[8] = {};

    struct WholeTextureState
    {
        void* instance = nullptr;
        uint32_t materialByType[32]{};
        void* textureByType[32]{};
    };
    WholeTextureState g_wholeTextures[8]{};

    void RememberChoices(void* cmo, uint32_t chrModel, const uint32_t* ids, uint32_t count)
    {
        if (!chrModel) return;
        if (!cmo) return;
        void* root = *reinterpret_cast<void**>(static_cast<uint8_t*>(cmo) + off::kOffCharComponentInstance);
        SettledChoices* slot = nullptr;
        for (SettledChoices& entry : g_settled)
            // A component has one current root/model. Retaining its obsolete roots
            // fills the small cache and lets the secondary preview evict the
            // primary's settled choices. Readers still require exact root/model.
            if (entry.component == cmo) { slot = &entry; break; }
        if (!slot)
            for (SettledChoices& entry : g_settled)
                if (!entry.chrModel) { slot = &entry; break; }
        if (!slot) slot = &g_settled[0];   // a client shows a handful of models; the oldest gives way

        const uint32_t settled = count < kMaxSettled ? count : kMaxSettled;
        bool moved = slot->component != cmo || slot->root != root ||
            slot->chrModel != chrModel || slot->count != settled;
        for (uint32_t i = 0; i < settled && !moved; ++i) moved = slot->ids[i] != ids[i];

        slot->component = cmo;
        slot->root = root;
        slot->chrModel = chrModel;
        slot->count = settled;
        for (uint32_t i = 0; i < settled; ++i) slot->ids[i] = ids[i];

        // The sheet's layers are read from these same choices, so a choice that moves invalidates the
        // painted sheet exactly as surely as it invalidates the geometry -- and nothing else will
        // notice, because the sheet is composed from a decision it does not own. Asked here rather
        // than by whoever changed a choice: the change can arrive from the panel or from the tables
        // settling differently, and this is the one place both of them pass through.
        if (moved)
            if (void* const component = cmo)
                *(static_cast<uint8_t*>(component) + off::kOffCharComponentRebuild) |=
                    off::kCharRebuildSheet;
    }

    bool Contains(const uint16_t* list, uint32_t count, uint16_t id)
    {
        for (uint32_t i = 0; i < count; ++i) if (list[i] == id) return true;
        return false;
    }

    void Append(uint16_t* list, uint32_t& count, uint16_t id)
    {
        if (count < kMaxOwned && !Contains(list, count, id)) list[count++] = id;
    }

    /**
     * @brief Asks the tables which pieces this model owns and which of them to stand on.
     * @param stem Model path, which is what race and sex are recovered from.
     * @param out  Filled on success.
     * @return false when the tables hold no model for this race and sex.
     */
    bool SelectionFor(void* cmo, const char* stem, Selection& out)
    {
        const WXL_AppearanceApi* tables = Appearance();
        uint32_t raceId = 0;
        uint32_t sex = 0;
        if (!tables) return false;
        uint32_t retailRace = 0;
        const bool alternate = wxl_modern_m2::AlternateFormIdentity(cmo, retailRace, sex);
        if (alternate && !wxl_modern_m2::RetailCharacterCanaryAllows(retailRace == 23 ? 12 : retailRace == 70 ? 17 : 30, sex)) return false;
        if (!alternate)
        {
            if (!RaceOf(stem, raceId, sex) || !wxl_modern_m2::RetailCharacterCanaryAllows(raceId, sex)) return false;
            retailRace = wxl_modern_m2::RetailCharacterRace(raceId);
        }
        const uint32_t chrModel = tables->ChrModelForRace(retailRace, sex);
        if (!chrModel) return false;
        out.chrModel = chrModel;

        out.optionCount = alternate ? wxl_modern_m2::AlternateOptionCount(retailRace, sex) : tables->OptionCount(chrModel);
        if (!alternate) wxl_modern_m2::CustomizeNoteModel(chrModel, raceId, sex);

        // Everything the customization can produce, in one question: handing the resolver every choice
        // at once returns the union of their geosets, which is exactly the set this decision owns.
        uint32_t all[kMaxChoices];
        uint32_t allCount = 0;
        for (uint32_t i = 0; i < out.optionCount; ++i)
        {
            WXL_ChrOption option{};
            if (!(alternate ? wxl_modern_m2::AlternateOptionAt(retailRace, sex, i, &option) : tables->OptionAt(chrModel, i, &option))) continue;
            const uint32_t choices = tables->ChoiceCount(option.id);
            for (uint32_t c = 0; c < choices; ++c)
            {
                if (allCount >= kMaxChoices) { out.choicesTruncated = true; break; }
                WXL_ChrChoice choice{};
                if (tables->ChoiceAt(option.id, c, &choice)) all[allCount++] = choice.id;
            }
        }

        WXL_Recipe recipe{};
        if (!tables->BuildForCharacter(retailRace, sex, all, allCount, &recipe)) return false;
        out.modelFileDataId = recipe.modelFileDataId;
        out.textureLayoutId = recipe.textureLayoutId;
        out.layerCount      = recipe.layerCount;
        ReportLayout(tables, out.textureLayoutId, chrModel);
        for (uint32_t i = 0; i < recipe.geosetCount; ++i) Append(out.owned, out.ownedCount, recipe.geosets[i]);

        // One option at a time for what to stand on. The character's own choices are out of reach --
        // the stock client keeps five legacy bytes and the table that mapped them onto modern choices
        // ships no rows -- so the option's OWN first choice stands in, which is the appearance the
        // tables themselves present as the starting point.
        //
        // Taking it plainly, and not the first choice that happens to name geometry the model carries.
        // An option's first choice is overwhelmingly its empty one, and the tables spell empty as a
        // geoset whose value is zero -- a beard of "None" is group 1 value 0. No model carries a
        // submesh for that, so a rule that skips choices contributing nothing the model carries skips
        // every one of those and lands on the first real beard instead. Applied across every option at
        // once, that is a character wearing a beard, sideburns, a moustache, earrings and a nose ring
        // that nobody asked for, which is the opposite of a plain one.
        //
        // Every choice settled here is also written down, because the sheet's layers have to come from
        // the SAME choices as the geometry. Deciding twice, independently, is how a hair mesh ends up
        // wearing another style's texture: both answers are defensible on their own and they are not
        // the same answer.
        // Logged once per ChrModel, not once per SelectionFor call: this runs every time the geoset
        // decision does, which is every frame the component needs it, and a line per option per frame
        // would be unreadable before it was useful.
        static uint32_t reportedOptions = 0;
        const bool reportThisModel = reportedOptions != chrModel;
        if (reportThisModel) reportedOptions = chrModel;
        const bool voidElfMale = stem && text::ContainsCI(stem, "voidelf\\male");
        bool voidElfMaleAdvancedChoice = false;

        for (uint32_t i = 0; i < out.optionCount; ++i)
        {
            WXL_ChrOption option{};
            if (!(alternate ? wxl_modern_m2::AlternateOptionAt(retailRace, sex, i, &option) : tables->OptionAt(chrModel, i, &option))) continue;

            // Taken by hand for this option, if the panel says so: the rule below is a stand-in for a
            // decision nobody can read out of the client, so anything deliberate outranks it.
            const int serverChoice = wxl_modern_m2::ServerAppearanceChoice(cmo, chrModel, option.id);
            const int chosen = serverChoice >= 0 ? serverChoice : wxl::game::world::CurrentMapId() < 0 ? wxl_modern_m2::CustomizeChoiceFor(chrModel, i) : -1;
            const uint32_t index = chosen >= 0 ? uint32_t(chosen) : 0;

            WXL_ChrChoice choice{};
            if (!tables->ChoiceAt(option.id, index, &choice)) continue;
            choice.id = wxl_modern_m2::dragon::RequiredHorns(choice.id);
            const char* optionName =
                tables->OptionName ? tables->OptionName(option.id) : nullptr;

            // The Void Elf male's modern root-skinned child combines its Demon Hunter horns and
            // hanging waist accessory. Retail contributes that child through the combined recipe
            // even when Horns, Blindfold and Tattoo all settle on their empty rows, so it appears a
            // moment after the correct root geosets and vanishes only after a model reload. Record
            // whether any advanced option was deliberately chosen; the combined attachment pass
            // below uses this to reject the default-empty child without disabling real selections.
            const char* choiceName =
                tables->ChoiceName ? tables->ChoiceName(choice.id) : nullptr;
            if (voidElfMale && optionName && choiceName &&
                (std::strcmp(optionName, "Horns") == 0 ||
                 std::strcmp(optionName, "Blindfold") == 0 ||
                 std::strcmp(optionName, "Tattoo") == 0) &&
                std::strcmp(choiceName, "None") != 0)
                voidElfMaleAdvancedChoice = true;

            // Female Orc keeps its cornea/eye-style shell as group 51: 5101 is both eyes, while
            // 5102/5103 are the two measured half meshes. Retail's Eyesight choices carry only
            // type-19 overlay materials, so the generic recipe has no geoset fact with which to
            // choose these otherwise-unclaimed variants. Map the option's explicit Both/Right/Left/
            // Neither names to the geometry the model itself supplies; group 33 remains the opaque
            // iris/sclera underneath.
            if (text::ContainsCI(stem, "orc\\female") && optionName &&
                std::strcmp(optionName, "Eyesight") == 0)
            {
                Append(out.owned, out.ownedCount, 5101);
                Append(out.owned, out.ownedCount, 5102);
                Append(out.owned, out.ownedCount, 5103);
                if (choiceName && std::strcmp(choiceName, "Both") == 0)
                    Append(out.chosen, out.chosenCount, 5101);
                else if (choiceName && std::strcmp(choiceName, "Right") == 0)
                    Append(out.chosen, out.chosenCount, 5102);
                else if (choiceName && std::strcmp(choiceName, "Left") == 0)
                    Append(out.chosen, out.chosenCount, 5103);
            }

            WXL_Recipe one{};
            if (tables->BuildForCharacter(retailRace, sex, &choice.id, 1, &one))
            {
                for (uint32_t g = 0; g < one.geosetCount; ++g)
                    Append(out.chosen, out.chosenCount, one.geosets[g]);
                for (uint32_t layer = 0; layer < one.layerCount; ++layer)
                    if (one.layers[layer].textureType == 19) out.hasEyeTexture = true;
                ReportLayers(one, choice.id);

                if (reportThisModel)
                {
                    char geosets[160] = "";
                    int at = 0;
                    for (uint32_t g = 0; g < one.geosetCount && at < int(sizeof geosets) - 8; ++g)
                    {
                        const uint32_t id = one.geosets[g];
                        const char* groupName = wxl_modern_m2::GeosetGroupName(id);
                        at += groupName
                            ? std::snprintf(geosets + at, sizeof geosets - at, " %u(%s)", id, groupName)
                            : std::snprintf(geosets + at, sizeof geosets - at, " %u", id);
                    }
                    WLOG_INFO("char-geosets:   option %u '%s' -> choice %u '%s'%s",
                              option.id, optionName ? optionName : "", choice.id,
                              choiceName ? choiceName : "", geosets);
                }
            }
            if (out.choiceCount < kMaxSettled) out.choices[out.choiceCount++] = choice.id;
        }

        // The UI generation is deliberately process-global and changes as different CMOs/races are
        // observed. Root-skinned children need a per-recipe generation instead: the ordered settled
        // choice ids are the complete input to BuildForCharacter and remain stable for an unchanged
        // CMO even while another model renders. FNV-1a keeps this key deterministic and nonzero.
        uint32_t recipeGeneration = 2166136261u;
        const auto mixGeneration = [&](uint32_t value) {
            for (uint32_t byte = 0; byte < 4; ++byte)
            {
                recipeGeneration ^= value & 0xFFu;
                recipeGeneration *= 16777619u;
                value >>= 8;
            }
        };
        mixGeneration(chrModel);
        for (uint32_t i = 0; i < out.choiceCount; ++i)
            mixGeneration(out.choices[i]);
        out.recipeGeneration = recipeGeneration ? recipeGeneration : 1u;

        // Resolve the settled choices together once as well. Related-choice elements are the table's
        // join between options (hair style + hair colour, eye colour + eye style); asking either
        // choice alone deliberately omits them. The per-option pass above remains useful for the
        // ordinary independent geometry and its diagnostics, while this combined pass adds only the
        // dependent geosets/material facts that become true for the actual selected combination.
        WXL_Recipe settled{};
        if (tables->BuildForCharacter(retailRace, sex, out.choices, out.choiceCount, &settled))
        {
            for (uint32_t g = 0; g < settled.geosetCount; ++g)
                Append(out.chosen, out.chosenCount, settled.geosets[g]);
            for (uint32_t layer = 0; layer < settled.layerCount; ++layer)
                if (settled.layers[layer].textureType == 19) out.hasEyeTexture = true;
            for (uint32_t i = 0; i < settled.attachedCount; ++i)
            {
                if (voidElfMale && !voidElfMaleAdvancedChoice &&
                    settled.attached[i].fileDataId == 7760207u)
                    continue;
                if (out.attachedCount >= kMaxAttached)
                {
                    out.attachmentsTruncated = true;
                    break;
                }
                out.attached[out.attachedCount++] = {
                    settled.attached[i].fileDataId,
                    settled.attached[i].geoset,
                };
            }
        }

        // A number of Retail races supply ordinary eye colour/style only as type-19 material. Their
        // option family can still claim group 33 through a special hidden/Transmog choice, causing
        // the visibility pass to clear 3301 while leaving hair, lashes, or effect shells around an
        // empty socket. Keep any explicit group-33 selection authoritative; otherwise a selected eye
        // material needs the model's opaque iris/sclera section on every race, not only Female Orc.
        if (out.hasEyeTexture)
        {
            bool choseEyeGroup = false;
            for (uint32_t i = 0; i < out.chosenCount && !choseEyeGroup; ++i)
                choseEyeGroup = out.chosen[i] / kGroupStride == 33;
            if (!choseEyeGroup) Append(out.chosen, out.chosenCount, 3301);
        }

        RememberChoices(cmo, chrModel, out.choices, out.choiceCount);
        // Some Retail customization families live entirely in root-skinned child models. They still
        // have a complete table recipe even when the main body owns no selectable geoset itself.
        return out.ownedCount != 0 || out.attachedCount != 0;
    }

    bool IsWholeTextureType(uint32_t textureType)
    {
        switch (textureType)
        {
        // Retail customization sources sampled as complete replaceable textures. Type 1 is the
        // composited character sheet and deliberately does not belong here.
        case 6: case 8: case 9: case 10: case 19: case 20: case 22: case 24:
            return true;
        default:
            return false;
        }
    }

    WholeTextureState& TextureStateFor(void* instance)
    {
        for (WholeTextureState& state : g_wholeTextures)
            if (state.instance == instance) return state;
        for (WholeTextureState& state : g_wholeTextures)
            if (!state.instance) { state.instance = instance; return state; }

        // Binding takes a separate engine reference (8252C5 calls Resource.AddRef). Release the
        // cache's LoadResource references on eviction; render contexts keep their own references.
        static uint32_t next = 0;
        WholeTextureState& state = g_wholeTextures[next++ % 8];
        for (void* texture : state.textureByType)
            if (texture) wxl::game::m2::ReleaseResource(texture);
        state = {};
        state.instance = instance;
        return state;
    }

    /** @brief Binds the selected Retail hair/eye/etc. textures to their native replaceable slots. */
    void BindWholeTextures(void* cmo, void* instance, const char* stem, const Selection& pick)
    {
        const WXL_AppearanceApi* tables = Appearance();
        const WXL_FdidApi* files = Files();
        uint32_t raceId = 0, sex = 0;
        if (!instance || !tables || !files || !pick.choiceCount) return;
        uint32_t retailRace = 0;
        if (!wxl_modern_m2::AlternateFormIdentity(cmo, retailRace, sex))
        {
            if (!RaceOf(stem, raceId, sex)) return;
            retailRace = wxl_modern_m2::RetailCharacterRace(raceId);
        }
        WXL_Recipe recipe{};
        if (!tables->BuildForCharacter(retailRace, sex,
                                       pick.choices, pick.choiceCount, &recipe))
            return;

        const WXL_TextureLayer* selected[32]{};
        WXL_Recipe gilneanEyes{};
        // Eye atlases are independent replaceable textures on this exact Human
        // mesh. Resolve their target using Human's layout, not Gilnean's legacy
        // body layout (which contains no modern eye target).
        if (retailRace == 23 && wxl_modern_m2::AlternateFormIdentity(cmo, retailRace, sex))
            tables->BuildForCharacter(1, sex, pick.choices, pick.choiceCount, &gilneanEyes);
        WXL_TextureLayer bloodElfBlindfoldFallback{};
        for (uint32_t i = 0; i < recipe.layerCount + gilneanEyes.layerCount; ++i)
        {
            const bool eyePass = i >= recipe.layerCount;
            const WXL_TextureLayer& layer = eyePass ? gilneanEyes.layers[i - recipe.layerCount] : recipe.layers[i];
            if (eyePass && layer.textureType != 19) continue;
            const bool dragon = retailRace == 52 || retailRace == 70;
            // Dragon wings and decorations have additional whole replaceable slots.
            const bool dragonWhole = dragon && (layer.textureType == 7 || layer.textureType == 25);
            if (layer.textureType >= 32 || (!IsWholeTextureType(layer.textureType) && !dragonWhole)) continue;
            const WXL_TextureLayer* previous = selected[layer.textureType];
            // Type 6 is a dynamically composed hair texture in Retail. Style/color intersections
            // can add a higher InferAlpha mask above the complete Blit colour sheet. This client can
            // bind only one replaceable texture there, so treating that partial mask as the whole
            // sheet produces the stretched red/blue strips seen on Female Orc Braids. Prefer the
            // complete Blit source for hair. Eyes use the same authored contract: Eye Color is the
            // complete Blit image, while Eyesight is a later Screen/InferAlpha overlay. Binding that
            // overlay alone produced cyan smoke around an otherwise empty socket. Prefer the complete
            // base for both whole-texture families; conditional Eye Style variants are themselves
            // Blit layers, so the latest compatible one still wins within that family.
            const bool needsCompleteBase = layer.textureType == 6 || layer.textureType == 19 ||
                (dragon && (layer.textureType == 7 || layer.textureType == 10));
            const bool betterCompleteBase = needsCompleteBase && layer.blendMode == 1 &&
                previous && previous->blendMode != 1;
            const bool sameWholeKind = !needsCompleteBase || !previous ||
                (layer.blendMode == 1) == (previous->blendMode == 1);
            if (!previous || betterCompleteBase ||
                (sameWholeKind && layer.layer >= previous->layer))
                selected[layer.textureType] = &layer;
        }

        // PTR keeps the modern Blood Elf blindfold atlas behind a related-choice join with two
        // special eye-colour rows. Ordinary creation eye colours therefore settle the blindfold
        // geometry but contribute no type-9 material, leaving the child white (or sampling the
        // unrelated legacy face layer from the composed body sheet). The Exp11 child models use the
        // newer atlas variants: one atlas contains every blindfold style and its geoset supplies the
        // UV selection. Bind that authored atlas whenever a non-None blindfold choice is active and
        // the settled recipe did not already provide a more specific type-9 texture.
        if (raceId == 10 && !selected[9])
        {
            bool hasBlindfold = false;
            for (uint32_t i = 0; i < pick.choiceCount && !hasBlindfold; ++i)
                hasBlindfold = (sex == 0 && pick.choices[i] >= 1721u && pick.choices[i] <= 1731u) ||
                               (sex == 1 && pick.choices[i] >= 1858u && pick.choices[i] <= 1868u);
            if (hasBlindfold)
            {
                bloodElfBlindfoldFallback = {
                    9u, sex == 0 ? 13u : 17u, 1u, 0xFFFFFFFFu,
                    sex == 0 ? 1104895u : 1104893u,
                };
                selected[9] = &bloodElfBlindfoldFallback;
            }
        }

        // Night Elf blindfold styles likewise gate type-9 materials on special
        // eye colours. Keep the selected child textured with ordinary eye colours.
        // TextureFileData verifies these material IDs as FDIDs 7758292/7758283.
        WXL_TextureLayer nightElfBlindfoldFallback{};
        if (raceId == 4 && !selected[9])
        {
            bool hasBlindfold = false;
            for (uint32_t i = 0; i < pick.choiceCount && !hasBlindfold; ++i)
                hasBlindfold = (sex == 0 && pick.choices[i] >= 790u && pick.choices[i] <= 800u) ||
                               (sex == 1 && pick.choices[i] >= 902u && pick.choices[i] <= 912u);
            if (hasBlindfold)
            {
                nightElfBlindfoldFallback = {9u, 0u, 1u, 0xFFFFFFFFu,
                    sex == 0 ? 1104903u : 1104901u};
                selected[9] = &nightElfBlindfoldFallback;
            }
        }

        // The legacy male Gilnean layout has no whole-hair target row for some
        // styles. Its HD mesh still samples type 6. Supply the authored palette
        // resource keyed by the selected Gilnean choice (verified TextureFileData),
        // not a Human modern colour ordinal or an inherited parent binding.
        WXL_TextureLayer gilneanHair{};
        uint32_t gilneanRace = 0, gilneanSex = 0;
        if (!selected[6] && wxl_modern_m2::AlternateFormIdentity(cmo, gilneanRace, gilneanSex) && gilneanRace == 23)
        {
            for (uint32_t i = 0; i < pick.choiceCount; ++i)
            {
                const uint32_t choice = pick.choices[i];
                uint32_t material = 0;
                if (gilneanSex == 0 && choice >= 2427 && choice <= 2431) material = 159727 + choice - 2427;
                if (gilneanSex == 1 && choice >= 2515 && choice <= 2519) material = 128788 + choice - 2515;
                if (material) { gilneanHair = {6, 2, 1, 0xFFFFFFFFu, material}; selected[6] = &gilneanHair; break; }
            }
        }
        WholeTextureState& state = TextureStateFor(instance);
        for (uint32_t textureType = 0; textureType < 32; ++textureType)
        {
            const WXL_TextureLayer* layer = selected[textureType];
            if (!layer || !layer->materialResourceId) continue;

            const bool changed = state.materialByType[textureType] != layer->materialResourceId ||
                                 !state.textureByType[textureType];
            const char* path = nullptr;
            void* texture = state.textureByType[textureType];
            if (changed)
            {
                path = files->ResolveMaterialTexture(layer->materialResourceId, textureType);
                if (!path || !*path) continue;
                __try { texture = wxl::game::m2::LoadResource(path); }
                __except (EXCEPTION_EXECUTE_HANDLER) { texture = nullptr; }
                if (!texture)
                {
                    WLOG_WARN("char-whole-texture: load failed type=%u material=%u path=%s",
                              textureType, layer->materialResourceId, path);
                    continue;
                }
            }
            bool bound = false;
            __try
            {
                wxl::game::m2::BindTexSlotType(instance, textureType, texture);
                bound = true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER) { bound = false; }
            if (!bound)
            {
                if (changed) wxl::game::m2::ReleaseResource(texture);
                continue;
            }
            if (changed && state.textureByType[textureType])
                wxl::game::m2::ReleaseResource(state.textureByType[textureType]);
            state.materialByType[textureType] = layer->materialResourceId;
            state.textureByType[textureType] = texture;
            if (changed)
                WLOG_INFO("char-whole-texture: bound type=%u material=%u path=%s instance=%p",
                          textureType, layer->materialResourceId, path, instance);
        }
    }
}

namespace wxl_modern_m2
{
    bool RaceOfModelPath(const char* path, uint32_t& chrRaceId, uint32_t& sex)
    {
        return RaceOf(path, chrRaceId, sex);
    }

    uint32_t SettledChoicesFor(void* cmo, uint32_t chrModel, uint32_t* out, uint32_t capacity)
    {
        if (!cmo || !chrModel || !out || !capacity) return 0;
        if (uint32_t count = GilneanPreviewChoices(cmo, chrModel, out, capacity)) return count;
        uint32_t serverCount = 0;
        if (ServerAppearanceChoices(cmo, chrModel, out, capacity, serverCount)) return serverCount;
        // World fallback must not reuse a preview or another unit's model-keyed result.
        if (wxl::game::world::CurrentMapId() >= 0) return 0;
        void* root = *reinterpret_cast<void**>(static_cast<uint8_t*>(cmo) + off::kOffCharComponentInstance);
        for (const SettledChoices& entry : g_settled)
        {
            if (entry.component != cmo || entry.root != root || entry.chrModel != chrModel) continue;
            const uint32_t count = entry.count < capacity ? entry.count : capacity;
            for (uint32_t i = 0; i < count; ++i) out[i] = entry.ids[i];
            return count;
        }
        return 0;
    }

    void ForgetCustomizationComponent(void* component, void* root)
    {
        for (auto& entry : g_settled) if (entry.component == component) entry = {};
        for (auto& entry : g_wholeTextures)
        {
            if (entry.instance != root) continue;
            for (void* texture : entry.textureByType)
                if (texture) wxl::game::m2::ReleaseResource(texture);
            entry = {};
        }
        ReleaseCustomizationAttachments(component);
    }

    void* CustomizationWholeTexture(void* rootInstance, uint32_t textureType)
    {
        if (!rootInstance || textureType >= 32) return nullptr;
        for (const WholeTextureState& state : g_wholeTextures)
            if (state.instance == rootInstance)
                return state.textureByType[textureType];
        return nullptr;
    }
}

namespace
{
    void ShowGeosets(void* instance, uint32_t idStart, uint32_t idEnd, uint32_t visible)
    {
        wxl::game::Native<off::M2_SetGeometryVisibleFn>(off::kSetGeometryVisible)(
            instance, nullptr, idStart, idEnd, visible);
    }

    bool BindFixedTuskarrFemale(void* cmo, void* instance, const char* stem)
    {
        if (!instance || !stem || !text::ContainsCI(stem, "tuskarrfemale_hd")) return false;

        uint32_t skinIndex = 0;
        const WXL_AppearanceApi* const tables = Appearance();
        const uint32_t chrModel = tables && tables->ChrModelForRace
            ? tables->ChrModelForRace(17, 0) : 0;
        const bool preview = wxl::game::world::CurrentMapId() < 0;
        if (preview) wxl_modern_m2::CustomizeNoteModel(chrModel, 14, 1);
        if (preview && chrModel)
        {
            const int selected = wxl_modern_m2::CustomizeChoiceFor(chrModel, 0);
            if (selected >= 0) skinIndex = static_cast<uint32_t>(selected);
        }

        const int serverSkin = wxl_modern_m2::ServerPrivateAppearanceChoice(cmo, 14, 1, wxl::appearance::PrivateSkin);
        if (serverSkin >= 0) skinIndex = uint32_t(serverSkin);
        // Only three of the extracted display skins are visually complete on this model. Keep their
        // original variant identities while exposing them as one compact synthetic selector.
        static constexpr uint32_t kSkinFileDataIds[] = { 4039141u, 4039142u, 4039147u };
        const uint32_t fileDataId = kSkinFileDataIds[
            skinIndex % (sizeof kSkinFileDataIds / sizeof kSkinFileDataIds[0])];
        char skinPath[96];
        std::snprintf(skinPath, sizeof skinPath,
                      "creature\\tuskarr\\tuskarrfemale_hd_%u.blp", fileDataId);
        void* texture = nullptr;
        __try { texture = wxl::game::m2::LoadResource(skinPath); }
        __except (EXCEPTION_EXECUTE_HANDLER) { texture = nullptr; }
        if (!texture)
        {
            WLOG_WARN("char-geosets: Tuskarr female skin failed to load: %s", skinPath);
            return true;
        }
        __try
        {
            wxl::game::m2::BindTexSlotType(instance, 11, texture);
            wxl::game::m2::BindTexSlotType(instance, 12, texture);
            wxl::game::m2::BindTexSlotType(instance, 13, texture);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            WLOG_WARN("char-geosets: fixed Tuskarr female skin binding faulted");
            return true;
        }
        static uint32_t reported = 0;
        if (reported != fileDataId)
        {
            reported = fileDataId;
            WLOG_INFO("char-geosets: Tuskarr female creature skin %u bound to slots 11-13",
                      fileDataId);
        }
        return true;
    }

    bool BindFixedMurlocFemale(void* cmo, void* instance, const char* stem)
    {
        if (!instance || !stem ||
            (!text::ContainsCI(stem, "murloc\\male\\murlochd") &&
             !text::ContainsCI(stem, "murloc\\female\\murlocfemale") &&
             !text::ContainsCI(stem, "murloc2maria")))
            return false;

        uint32_t race = 0, sex = 0;
        if (wxl::game::world::CurrentMapId() >= 0 && cmo)
        {
            race = *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(cmo) + off::kOffCharComponentRace);
            sex = *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(cmo) + off::kOffCharComponentSex);
        }
        else wxl_modern_m2::CustomizeIdentity(race, sex);
        if (race != 28 || sex != 1) return false;

        constexpr uint32_t kChoiceCount = 5;
        constexpr uint32_t kOutfitTextureType = 31;
        const int savedSkin = wxl_modern_m2::ServerPrivateAppearanceChoice(cmo, 28, 1, wxl::appearance::PrivateSkin);
        const int savedOutfit = wxl_modern_m2::ServerPrivateAppearanceChoice(cmo, 28, 1, wxl::appearance::PrivateOutfit);
        const bool preview = wxl::game::world::CurrentMapId() < 0;
        const uint32_t skin = savedSkin >= 0 ? uint32_t(savedSkin) : (preview ? wxl_modern_m2::MurlocFemaleSkin() % kChoiceCount : 0);
        const uint32_t outfit = savedOutfit >= 0 ? uint32_t(savedOutfit) : (preview ? wxl_modern_m2::MurlocFemaleOutfit() % kChoiceCount : 0);
        const uint32_t choices[] = { skin, outfit };
        const uint32_t types[] = { 11u, kOutfitTextureType };
        const char* const labels[] = { "skin", "outfit" };
        WholeTextureState& state = TextureStateFor(instance);
        for (uint32_t i = 0; i < 2; ++i)
        {
            const uint32_t key = 0x4D550000u | (i << 8) | choices[i];
            void* texture = state.textureByType[types[i]];
            const bool changed = state.materialByType[types[i]] != key || !texture;
            char path[96]{};
            if (changed)
            {
                std::snprintf(path, sizeof path,
                              "creature\\murloc2maria\\murloc2_%s_%02u.blp",
                              labels[i], choices[i]);
                __try { texture = wxl::game::m2::LoadResource(path); }
                __except (EXCEPTION_EXECUTE_HANDLER) { texture = nullptr; }
                if (!texture)
                {
                    WLOG_WARN("char-geosets: Murloc female %s failed to load: %s",
                              labels[i], path);
                    continue;
                }
            }
            __try { wxl::game::m2::BindTexSlotType(instance, types[i], texture); }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                WLOG_WARN("char-geosets: Murloc female %s binding faulted", labels[i]);
                continue;
            }
            state.materialByType[types[i]] = key;
            state.textureByType[types[i]] = texture;
            if (changed)
                WLOG_INFO("char-geosets: Murloc female %s %u bound to slot %u path=%s",
                          labels[i], choices[i], types[i], path);
        }
        return true;
    }

    void BindLegacyBrokenHair(void* component, void* instance, const char* stem)
    {
        if (!instance || !stem ||
            (!text::ContainsCI(stem, "broken\\male") &&
             !text::ContainsCI(stem, "broken\\female")))
            return;

        const uint32_t sex = text::ContainsCI(stem, "broken\\female") ? 1 : 0;
        const int saved = wxl_modern_m2::ServerPrivateAppearanceChoice(
            component, 23, sex, wxl::appearance::PrivateHairColor);
        const uint32_t color = saved >= 0 ? uint32_t(saved) :
            (wxl::game::world::CurrentMapId() < 0 ? wxl_modern_m2::LegacyBrokenHairColor() % 10 : 0);
        // Legacy CharSections owns the Broken selector, while the HD hair mesh still samples Retail's
        // whole-hair replaceable slot. Bind the matching extracted palette texture explicitly instead
        // of leaving the mesh on its multi-colour fallback material.
        const uint32_t key = 0xB6000000u | color;
        WholeTextureState& state = TextureStateFor(instance);
        void* texture = state.textureByType[6];
        const bool changed = state.materialByType[6] != key || !texture;
        char path[64]{};
        if (changed)
        {
            std::snprintf(path, sizeof path, "character\\broken\\hair00_%02u.blp", color);
            __try { texture = wxl::game::m2::LoadResource(path); }
            __except (EXCEPTION_EXECUTE_HANDLER) { texture = nullptr; }
            if (!texture)
            {
                WLOG_WARN("char-geosets: Broken hair colour failed to load: %s", path);
                return;
            }
        }
        __try { wxl::game::m2::BindTexSlotType(instance, 6, texture); }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            WLOG_WARN("char-geosets: Broken hair colour binding faulted");
            return;
        }
        state.materialByType[6] = key;
        state.textureByType[6] = texture;
        if (changed)
            WLOG_INFO("char-geosets: Broken hair colour %u bound to replaceable slot 6 path=%s",
                      color, path);
    }

    /**
     * @brief Shows exactly the geosets a recipe names, plus one variant of every group past the stock
     *        ceiling the recipe says nothing about.
     * @param skin Live skin, read for which ids those unclaimed groups actually offer.
     * @return false when no recipe could be built, leaving the caller on the numbering heuristic.
     */
    bool ShowRecipe(void* cmo, void* instance, const wxl::game::m2::M2SkinProfile* skin,
                    const char* stem)
    {
        // Race 28 female is intentionally a creature actor rather than a playable ChrModel. Its
        // display texture occupies replaceable slot 11; the fixed type-0 textures stay authored in
        // the model. Leave its two creature sections to the bounded fallback visibility decision.
        if (BindFixedMurlocFemale(cmo, instance, stem))
        {
            wxl_modern_m2::QueueCustomizationAttachments(cmo, instance, 0, 0, nullptr, 0);
            return false;
        }

        // Female Tuskarr is deliberately a creature model. It has no female playable ChrModel and
        // declares creature skin slots 11-13, so applying race 17's male recipe both leaves it white
        // and mutates unrelated geosets. Bind a verified creature display skin, then deliberately
        // fall through to the one-per-group heuristic so stock character hiding cannot remove limbs.
        if (BindFixedTuskarrFemale(cmo, instance, stem))
        {
            wxl_modern_m2::QueueCustomizationAttachments(cmo, instance, 0, 0, nullptr, 0);
            return false;
        }

        Selection pick{};
        const bool built = SelectionFor(cmo, stem, pick);

        // Void Elf male carries the Demon Hunter horn variants in the main body model, but the
        // all-choice DB2 union does not report that group as owned when its settled choice is the
        // empty "None" row. Treat the model's authored horn group as customization-owned here: the
        // ordinary chosen pass below can still re-show a real selection, while None hides it. Group
        // 18 is deliberately not claimed; its lowest section is the required waist seam, not a DH
        // accessory.
        if (built && stem && text::ContainsCI(stem, "voidelf\\male"))
        {
            for (uint32_t i = 0; i < skin->submeshCount; ++i)
            {
                const uint16_t id = skin->submeshes[i].skinSectionId;
                const uint32_t group = id / kGroupStride;
                if (group == 24)
                    Append(pick.owned, pick.ownedCount, id);
            }
        }

        static const void* reported = nullptr;
        if (stem != reported)
        {
            reported = stem;
            if (built)
            {
                // The stem is the name the client ASKED for, which the HD switch answers with a
                // different file; naming only that would read as though the stock model were loaded.
                // The recipe's own model is what settles whether the tables and the geometry are
                // talking about the same thing.
                const char* served = wxl_modern_m2::ResolveModel(pick.modelFileDataId);
                // The layer count is the one from the pass that asked for EVERY choice at once, so it
                // is the whole customization's worth of layers and not this appearance's. Said plainly
                // because the number is large and would otherwise read as one character's texture
                // stack; whatever composites these will have to gather them per chosen choice.
                // Named, not just counted. A geoset's group says which texture it expects, and a
                // group whose texture is equipment the character is not wearing draws the sheet it
                // was last handed instead -- a cloak panel wearing a face is the same count as a
                // cloak panel wearing a cloak.
                char shown[160] = "";
                int at = 0;
                for (uint32_t i = 0; i < pick.chosenCount && at < int(sizeof shown) - 8; ++i)
                {
                    const uint32_t id = pick.chosen[i];
                    const char* groupName = wxl_modern_m2::GeosetGroupName(id);
                    at += groupName
                        ? std::snprintf(shown + at, sizeof shown - at, " %u(%s)", id, groupName)
                        : std::snprintf(shown + at, sizeof shown - at, " %u", id);
                }

                WLOG_INFO("char-geosets: requested '%s' | model %u '%s' | %u option(s) own %u"
                          " geoset(s), standing on %u:%s | layout %u, %u texture layer(s) across"
                          " every choice%s",
                          stem, pick.modelFileDataId, served ? served : "(unresolved)",
                          pick.optionCount, pick.ownedCount, pick.chosenCount, shown,
                          pick.textureLayoutId, pick.layerCount,
                          pick.choicesTruncated ? "  <-- choice list truncated, owns more than this"
                                                : "");
            }
            else
                WLOG_WARN("char-geosets: '%s' has no entry in the tables; falling back to one piece"
                          " per group", stem ? stem : "(no stem)");
        }
        if (!built)
        {
            wxl_modern_m2::QueueCustomizationAttachments(
                cmo, instance, 0, 0, nullptr, 0);
            return false;
        }

        wxl_modern_m2::QueueCustomizationAttachments(
            cmo, instance, pick.chrModel, pick.recipeGeneration,
            pick.attached, pick.attachedCount);
        if (pick.attachmentsTruncated)
            WLOG_WARN("char-geosets: customization attachment list truncated model=%u at %u rows",
                      pick.chrModel, kMaxAttached);

        BindWholeTextures(cmo, instance, stem, pick);

        // Above the stock ceiling nothing is ever decided at all. The stock decision opens by hiding
        // one band, and every id it shows afterwards -- its own slots, and the bands it reads off the
        // equipped items -- lands inside that same band. So a group numbered past it was never hidden,
        // was never chosen, and draws every variant it has, all at once, stacked on itself.
        ShowGeosets(instance, kStockIdCeiling + 1, kModernIdCeiling, 0);

        // Group zero is modern hair. The stock pass interprets the same 1..99 band as several legacy
        // hair/facial slots and can leave more than one modern hairstyle standing. Female Orc made
        // the failure especially visible: the selected style, a bald scalp, and a long style were
        // submitted together, making eye/face layers appear to live inside the hair. Start this one
        // group from a known state; the recipe-selected entries below put its one intended style back.
        ShowGeosets(instance, 1, kGroupStride - 1, 0);

        // Clearing the band is only half of it, and the missing half is not symmetric. A group up
        // there is not automatically a customization group: a modern race model also keeps plain body
        // geometry in groups no option will ever name, and which group that is differs by race. Leave
        // those cleared and the body loses a piece outright -- a torso on one race, part of a face on
        // the next -- because their one variant was never a choice and so nothing puts it back.
        //
        // So the band is split by what the tables CLAIM. A group they name is decided by the recipe
        // below. A group they do not name gets exactly one variant: the lowest id the model carries in
        // it, which is the one the model itself lists first and the only defensible pick with no
        // choice to read. One is the right answer for both kinds of group; the two kinds just disagree
        // about who chooses it.
        for (uint32_t group = kFirstModernGroup; group < kLastModernGroup; ++group)
        {
            bool claimed = false;
            for (uint32_t i = 0; i < pick.ownedCount && !claimed; ++i)
                claimed = pick.owned[i] / kGroupStride == group;
            if (claimed) continue;

            uint32_t lowest = 0;
            for (uint32_t i = 0; i < skin->submeshCount; ++i)
            {
                const uint32_t id = skin->submeshes[i].skinSectionId;
                if (id <= kStockIdCeiling || id / kGroupStride != group) continue;
                if (!lowest || id < lowest) lowest = id;
            }
            if (lowest) ShowGeosets(instance, lowest, lowest, 1);
        }

        // Below the ceiling the tables describe CUSTOMIZATION, not equipment: they know which face and
        // which tusks, and nothing about the boots this character happens to be wearing. The stock
        // decision gets the equipment right, because it reads what is actually equipped -- so anything
        // the tables do not claim is left exactly as that decision left it.
        //
        // Which is why the split is membership and not a number. Every id the customization can
        // produce is hidden unless it was picked; ids outside that set are never touched. Id 0 is
        // excluded on its own terms: the base body is not a choice, it is what the choices are made
        // ON, and some races carry a second piece under the same id that has to follow it.
        for (uint32_t i = 0; i < pick.ownedCount; ++i)
        {
            const uint16_t id = pick.owned[i];
            if (id == 0 || Contains(pick.chosen, pick.chosenCount, id)) continue;
            ShowGeosets(instance, id, id, 0);
        }
        // Dragon's None choice names sentinel 1200, but native equipment may
        // enable real tabard panels 1201..1214, which sample clothing slot 26.
        // Some variants are absent from the customization union, so hiding only
        // individually owned IDs leaves white chest panels. Clear the group;
        // only an explicitly selected real variant may be restored below.
        if (pick.chrModel == 89) ShowGeosets(instance, 1200, 1299, 0);
        for (uint32_t i = 0; i < pick.chosenCount; ++i)
            if (pick.chosen[i] != 0) ShowGeosets(instance, pick.chosen[i], pick.chosen[i], 1);

        // Eye Color choice 9313 owns 1701, the Death Knight eye-effect shell. Retail availability
        // filters keep that effect class-only, but the raw customization table exposes the choice to
        // every class. Gate just this authored variant from the Glue's selected class; elemental
        // 1702..1705 Primalist variants remain ordinary selectable eye colors.
        ShowGeosets(instance, 1701, 1701, wxl_modern_m2::CustomizeClass() == 6 ? 1 : 0);

        // Modern heads are commonly HeadSwap geometry rather than part of group zero. Human, Dwarf,
        // Night Elf, Earthen, and Female Orc all demonstrated the same legacy collision: the stock
        // decision can leave a small neck cap visible while hiding the complete face. If the recipe
        // names no HeadSwap, choose the most complete authored section by vertex count. This remains
        // inside the base character model and does not touch transmog/equipment child geosets.
        constexpr uint32_t kHeadSwapGroup = 32;
        bool choseHeadSwap = false;
        for (uint32_t i = 0; i < pick.chosenCount && !choseHeadSwap; ++i)
            choseHeadSwap = pick.chosen[i] / kGroupStride == kHeadSwapGroup;
        if (!choseHeadSwap)
        {
            uint32_t head = 0;
            uint32_t mostVertices = 0;
            for (uint32_t i = 0; i < skin->submeshCount; ++i)
            {
                const auto& section = skin->submeshes[i];
                if (section.skinSectionId / kGroupStride != kHeadSwapGroup ||
                    section.vertexCount <= mostVertices)
                    continue;
                head = section.skinSectionId;
                mostVertices = section.vertexCount;
            }
            if (head)
            {
                ShowGeosets(instance, kHeadSwapGroup * kGroupStride,
                            (kHeadSwapGroup + 1) * kGroupStride - 1, 0);
                ShowGeosets(instance, head, head, 1);
            }
        }
        return true;
    }

    /**
     * @brief Shows one value per geoset group living past the stock ceiling, having first hidden the
     *        whole range. Used only for a race the tables cannot name at all.
     * @param instance Model instance owning the visibility array.
     * @param skin     Live skin profile, read for the ids its submeshes actually carry.
     */
    void ShowOnePerModernGroup(void* instance, const wxl::game::m2::M2SkinProfile* skin)
    {
        ShowGeosets(instance, kStockIdCeiling, kModernIdCeiling, 0);

        // Group 0 is the one place inside the stock range where the two vocabularies disagree about
        // what the numbers MEAN, not merely about how high they go. The stock decision reads 1..99 as
        // hair styles and names several at once from its slots, which is right for the model it was
        // written against. On a modern race model those ids are head pieces that all occupy the same
        // space, so naming several draws several heads. Id 0 itself is the torso and stays as the
        // stock decision left it.
        ShowGeosets(instance, 1, kGroupStride - 1, 0);
        uint32_t head = 0;
        for (uint32_t i = 0; i < skin->submeshCount; ++i)
        {
            const uint32_t id = skin->submeshes[i].skinSectionId;
            if (id == 0 || id >= kGroupStride) continue;
            if (!head || id < head) head = id;
        }
        if (head) ShowGeosets(instance, head, head, 1);

        // The lowest id in a group is the one the model itself lists first, and with no recorded
        // choice it is the only defensible pick: it keeps the silhouette complete (one nose, one pair
        // of feet) instead of either stacking every variant or dropping the group entirely.
        for (uint32_t group = kFirstModernGroup; group < kLastModernGroup; ++group)
        {
            uint32_t lowest = 0;
            for (uint32_t i = 0; i < skin->submeshCount; ++i)
            {
                const uint32_t id = skin->submeshes[i].skinSectionId;
                if (id / kGroupStride != group) continue;
                if (!lowest || id < lowest) lowest = id;
            }
            if (lowest) ShowGeosets(instance, lowest, lowest, 1);
        }
    }

    /// Geoset id to show alone, or kNoIsolation for the normal decision. Seeded at install from
    /// WXL_M2_ISOLATE_GEOSET so a bisection can be underway before the world is up, and moved from
    /// there by the panel: every piece of a race model can be looked at on its own until the one that
    /// misbehaves is named.
    uint32_t g_isolate = wxl_modern_m2::kNoIsolation;

    /// The geoset ids the last decision actually left standing, in the order they were shown. The
    /// panel walks this rather than the whole skin so the list is what is on screen right now, which
    /// is the only list worth stepping through.
    constexpr uint32_t kMaxShown = 64;
    uint16_t g_shown[kMaxShown]{};
    uint32_t g_shownCount = 0;

    /**
     * @brief Restores the modern model's base body after the stock legacy geoset pass hides id 0.
     *
     * This writes the live visibility array by section index. Going back through the stock range
     * primitive made the right request but left another legacy interpretation between the modern
     * section and the final array; the array is the contract OptimizeVisibleGeometry consumes.
     */
    void EnsureBaseBodyVisible(void* instance, const wxl::game::m2::M2SkinProfile* skin)
    {
        auto* const shown =
            *reinterpret_cast<uint32_t**>(static_cast<uint8_t*>(instance) + off::kOffInstSectionVisible);
        if (!shown) return;

        bool changed = false;
        for (uint32_t i = 0; i < skin->submeshCount; ++i)
        {
            if (skin->submeshes[i].skinSectionId != 0 || shown[i]) continue;
            shown[i] = 1;
            changed = true;
        }
        if (changed)
            wxl::game::Native<off::M2_UnoptimizeVisibleGeometryFn>(off::kUnoptimizeVisibleGeometry)(
                instance, nullptr);
    }

    void CollectShown(void* instance, const wxl::game::m2::M2SkinProfile* skin)
    {
        const auto* const shown =
            *reinterpret_cast<uint32_t* const*>(static_cast<uint8_t*>(instance) + off::kOffInstSectionVisible);
        if (!shown) return;

        // Read back rather than record along the way: the pieces come from two decisions, the stock
        // one and ours, and only the array knows what survived both.
        g_shownCount = 0;
        for (uint32_t i = 0; i < skin->submeshCount && g_shownCount < kMaxShown; ++i)
        {
            if (!shown[i]) continue;
            const uint16_t id = skin->submeshes[i].skinSectionId;
            bool already = false;
            for (uint32_t k = 0; k < g_shownCount; ++k)
                if (g_shown[k] == id) { already = true; break; }
            if (!already) g_shown[g_shownCount++] = id;
        }
    }

    /**
     * @brief One bounded readback of the geometry the finished visibility decision will submit.
     *
     * It names section indices as well as ids because batches address the former. That distinguishes
     * "body hidden" from "body visible but no batch points at it" without changing the rendered
     * appearance for another diagnostic run.
     */
    void ReportVisibleGeometry(void* instance, void* model,
                               const wxl::game::m2::M2SkinProfile* skin)
    {
        static const void* reportedModel = nullptr;
        if (model == reportedModel) return;
        reportedModel = model;

        const auto* const shown =
            *reinterpret_cast<uint32_t* const*>(static_cast<uint8_t*>(instance) + off::kOffInstSectionVisible);
        if (!shown) return;

        uint32_t visibleCount = 0, bodySections = 0, bodyBatches = 0;
        for (uint32_t i = 0; i < skin->submeshCount; ++i)
        {
            if (shown[i]) ++visibleCount;
            if (skin->submeshes[i].skinSectionId == 0) ++bodySections;
        }
        for (uint32_t i = 0; i < skin->batchCount; ++i)
        {
            const uint32_t section = skin->batches[i].skinSectionIndex;
            if (section < skin->submeshCount && skin->submeshes[section].skinSectionId == 0)
                ++bodyBatches;
        }

        WLOG_INFO("char-geometry: model %p skin vtx=%u idx=%u sections=%u batches=%u; visible=%u;"
                  " base-body sections=%u batches=%u",
                  model, skin->vertexCount, skin->indexCount, skin->submeshCount, skin->batchCount,
                  visibleCount, bodySections, bodyBatches);

        const auto* const header = wxl::game::m2::M2Model(model).GetHeader();
        auto* const effects = *reinterpret_cast<void***>(
            static_cast<uint8_t*>(model) + off::kOffModelSubMeshCopy);
        auto* const sharedTextures = *reinterpret_cast<void***>(
            static_cast<uint8_t*>(model) + 0x174);
        void* const instanceTexture = *reinterpret_cast<void**>(
            static_cast<uint8_t*>(instance) + off::kOffInstTexBinding);
        void* const optimized = *reinterpret_cast<void**>(
            static_cast<uint8_t*>(instance) + off::kOffInstGeometryCtx);
        void* const renderObjects = *reinterpret_cast<void**>(
            static_cast<uint8_t*>(instance) + off::kOffInstRenderObjArray);
        WLOG_INFO("char-geometry: finalized effects=%p sharedTextures=%p instanceTexture=%p"
                  " optimized=%p renderObjects=%p alphaBase=%.6f alphaStage=%.6f",
                  effects, sharedTextures, instanceTexture, optimized, renderObjects,
                  *reinterpret_cast<const float*>(
                      static_cast<const uint8_t*>(instance) + off::kOffInstAlphaBase),
                  *reinterpret_cast<const float*>(
                      static_cast<const uint8_t*>(instance) + off::kOffInstAlphaStage));

        uint32_t sectionLines = 0;
        for (uint32_t i = 0; i < skin->submeshCount && sectionLines < 64; ++i)
        {
            if (!shown[i] && skin->submeshes[i].skinSectionId != 0) continue;
            const auto& section = skin->submeshes[i];
            const uint32_t fullIndexStart =
                (static_cast<uint32_t>(section.level) << 16) | section.indexStart;
            WLOG_INFO("char-geometry: section[%u] id=%u visible=%u vtx=%u+%u idx=%u+%u"
                      " bones=%u combo=%u",
                      i, section.skinSectionId, shown[i] ? 1u : 0u,
                      section.vertexStart, section.vertexCount, fullIndexStart, section.indexCount,
                      section.boneCount, section.boneComboIndex);
            ++sectionLines;
        }

        uint32_t batchLines = 0;
        for (uint32_t i = 0; i < skin->batchCount && batchLines < 64; ++i)
        {
            const auto& batch = skin->batches[i];
            const uint32_t sectionIndex = batch.skinSectionIndex;
            if (sectionIndex >= skin->submeshCount) continue;
            const bool body = skin->submeshes[sectionIndex].skinSectionId == 0;
            if (!body && !shown[sectionIndex]) continue;

            uint32_t textureSlot = 0xFFFFFFFFu;
            void* sharedTexture = nullptr;
            if (header && header->textureCombos.offset
                && batch.textureComboIndex < header->textureCombos.count)
            {
                textureSlot = reinterpret_cast<const uint16_t*>(
                    static_cast<uintptr_t>(header->textureCombos.offset))[batch.textureComboIndex];
                if (sharedTextures && textureSlot < header->textures.count)
                    sharedTexture = sharedTextures[textureSlot];
            }

            uint32_t weightIndex = 0xFFFFFFFFu;
            if (header && header->textureWeightCombos.offset
                && batch.textureWeightComboIndex < header->textureWeightCombos.count)
                weightIndex = reinterpret_cast<const uint16_t*>(
                    static_cast<uintptr_t>(header->textureWeightCombos.offset))[
                        batch.textureWeightComboIndex];

            WLOG_INFO("char-geometry: batch[%u] section=%u id=%u visible=%u geoset=%u shader=%#x"
                      " material=%u textures=%u effect=%p texSlot=%u sharedTex=%p weight=%u",
                      i, sectionIndex, skin->submeshes[sectionIndex].skinSectionId,
                      shown[sectionIndex] ? 1u : 0u, batch.geosetIndex, batch.shaderId,
                      batch.materialIndex, batch.textureCount, effects ? effects[i] : nullptr,
                      textureSlot, sharedTexture, weightIndex);
            ++batchLines;
        }
    }
}

namespace wxl_modern_m2
{
    uint32_t ShownGeosets(uint16_t* out, uint32_t capacity)
    {
        if (!out || !capacity) return 0;
        const uint32_t count = g_shownCount < capacity ? g_shownCount : capacity;
        for (uint32_t i = 0; i < count; ++i) out[i] = g_shown[i];
        return count;
    }

    uint32_t IsolatedGeoset() { return g_isolate; }

    void SetIsolatedGeoset(uint32_t id) { g_isolate = id; }
}

namespace
{
    // The private legacy female actor is not routed through the modern recipe pass.
    // Its stock decision hides all of group 2, leaving two actual openings in the
    // forehead. Source/live mesh comparison identifies 202 as the complete horn pair;
    // 201 is a flat cap and 203 is the alternate pair. This is creation-only until
    // an independently persisted horn choice exists; the separate tendril axis remains native.
    bool RestoreLegacyBrokenFemaleHorns(void* component, void* instance, void* model)
    {
        if (!component || !instance || !model)
            return false;
        const auto* bytes = static_cast<const uint8_t*>(component);
        if (*reinterpret_cast<const uint32_t*>(bytes + off::kOffCharComponentRace) != 23 ||
            *reinterpret_cast<const uint32_t*>(bytes + off::kOffCharComponentSex) != 1)
            return false;
        const char* stem = wxl::game::m2::M2Model(model).GetPathStem();
        if (!stem || !text::ContainsCI(stem, "broken\\female\\brokenfemale")) return false;
        const auto* skin = wxl::game::m2::M2Model(model).GetSkin();
        if (!skin || !skin->submeshes) return false;
        auto* shown = *reinterpret_cast<uint32_t**>(
            static_cast<uint8_t*>(instance) + off::kOffInstSectionVisible);
        if (!shown) return false;
        const bool inWorld = wxl::game::world::CurrentMapId() >= 0;
        const int savedHorn = wxl_modern_m2::ServerPrivateAppearanceChoice(component,23,1,wxl::appearance::PrivateHornStyle);
        const uint32_t horn = savedHorn >= 0 ? uint32_t(savedHorn) : inWorld ? 1u : wxl_modern_m2::LegacyBrokenHornStyle();
        const uint32_t selected = 201 + horn % 3;
        bool hasSelected = false;
        for (uint32_t i = 0; i < skin->submeshCount; ++i)
            if (skin->submeshes[i].skinSectionId == selected) hasSelected = true;
        if (!hasSelected) return false;
        bool changed = false;
        for (uint32_t i = 0; i < skin->submeshCount; ++i)
        {
            const uint32_t id = skin->submeshes[i].skinSectionId;
            if (id < 201 || id > 203) continue;
            const uint32_t wanted = id == selected ? 1 : 0;
            if (shown[i] != wanted) { shown[i] = wanted; changed = true; }
        }
        if (!changed) return false;
        wxl::game::Native<off::M2_UnoptimizeVisibleGeometryFn>(off::kUnoptimizeVisibleGeometry)(
            instance, nullptr);
        wxl::game::Native<off::M2_OptimizeVisibleGeometryFn>(off::kOptimizeVisibleGeometry)(
            instance, nullptr);
        return true;
    }

    bool ApplyCustomizationGeometry(void* component)
    {
        if (!component) return false;

        auto* const bytes = static_cast<uint8_t*>(component);
        void* const instance = *reinterpret_cast<void**>(bytes + off::kOffCharComponentInstance);
        if (!instance)
        {
            wxl_modern_m2::QueueCustomizationAttachments(
                component, nullptr, 0, 0, nullptr, 0);
            return false;
        }

        void* const model = *reinterpret_cast<void**>(
            static_cast<uint8_t*>(instance) + off::kOffInstShared);
        if (!model || !wxl::modern::assets::m2::IsNativeLoaded(model))
        {
            RestoreLegacyBrokenFemaleHorns(component, instance, model);
            wxl_modern_m2::QueueCustomizationAttachments(
                component, instance, 0, 0, nullptr, 0);
            return false;
        }

        auto* skin = wxl::game::m2::M2Model(model).GetSkin();
        if (!skin || !skin->submeshes || !skin->submeshCount)
        {
            wxl_modern_m2::QueueCustomizationAttachments(
                component, instance, 0, 0, nullptr, 0);
            return false;
        }

        if (g_isolate == wxl_modern_m2::kNoIsolation)
        {
            // The tables first; the numbering heuristic only while a race has no recipe to give.
            const char* const stem = wxl::game::m2::M2Model(model).GetPathStem();
            if (!ShowRecipe(component, instance, skin, stem))
            {
                ShowOnePerModernGroup(instance, skin);
                // Broken female's 402..404 family is torso/arm-local, despite the private DBC's
                // facial-style values previously making it look like a horn family. Do not force it.
                // The actual head-local insert is 302, while each selectable crest is a paired
                // same-id section in 1501..1506. Preserve a valid stock-selected crest so its insert
                // and shell stay together, falling back to the model's first complete authored pair.
                if (stem && text::ContainsCI(stem, "broken\\female"))
                {
                    uint32_t crest = 1502;
                    const auto* const slots = reinterpret_cast<const uint32_t*>(
                        static_cast<const uint8_t*>(component) +
                        off::kOffCharComponentGeosetSlots);
                    for (uint32_t i = 0; i < off::kCharComponentGeosetSlotCount; ++i)
                        // 1501 is only one side of the forehead insert. Every complete selectable
                        // pair starts at 1502 and carries two symmetric socket pieces.
                        if (slots[i] >= 1502 && slots[i] <= 1506) crest = slots[i];
                    ShowGeosets(instance, 1501, 1506, 0);
                    ShowGeosets(instance, crest, crest, 1);
                    ShowGeosets(instance, 302, 302, 1);
                    // The paired insert batches use replaceable texture type 2, but this old
                    // character model has no object-skin resource of its own. Reuse the already-owned
                    // composed body sheet; leaving type 2 unbound submits the insert geometry fully
                    // transparent and looks exactly like two sockets cut through the crest.
                    void* const bodySheet = *reinterpret_cast<void**>(
                        static_cast<uint8_t*>(instance) + off::kOffInstTexBinding);
                    if (bodySheet)
                    {
                        __try { wxl::game::m2::BindTexSlotType(instance, 2, bodySheet); }
                        __except (EXCEPTION_EXECUTE_HANDLER) {}
                    }
                }
                // Creature Tuskarr keeps two authored base pieces under section id zero. Stock
                // character visibility can clear either piece, so both must come back.
                if (stem && text::ContainsCI(stem, "tuskarrfemale_hd"))
                {
                    ShowGeosets(instance, 0, 0, 1);
                    // Unlike a playable character, this creature model keeps its authored arms and
                    // hands in body-variant group 3. The generic fallback deliberately leaves groups
                    // below the stock ceiling to the stock character recipe, but that recipe has no
                    // creature-body slot and clears the whole group. Variant 301 is the first complete
                    // authored body (both of its same-id sections are required).
                    ShowGeosets(instance, 301, 301, 1);
                    // This creature uses 1101 and 1102 as companion pieces, not exclusive character
                    // variants. The generic one-per-group fallback hid 1102 and removed its limbs.
                    ShowGeosets(instance, 1102, 1102, 1);
                }
            }
            BindLegacyBrokenHair(component, instance, stem);
        }
        else
        {
            wxl_modern_m2::QueueCustomizationAttachments(
                component, instance, 0, 0, nullptr, 0);
            // Everything off, one piece back on. A model whose every measurable property is correct
            // and which still draws wrong is not going to be explained by measuring those properties
            // again; it is going to be explained by looking at one piece at a time until one of them
            // is the one that misbehaves.
            ShowGeosets(instance, 0, kModernIdCeiling, 0);
            ShowGeosets(instance, g_isolate, g_isolate, 1);
        }
        EnsureBaseBodyVisible(instance, skin);
        CollectShown(instance, skin);
        ReportVisibleGeometry(instance, model, skin);
        wxl::game::Native<off::M2_OptimizeVisibleGeometryFn>(off::kOptimizeVisibleGeometry)(
            instance, nullptr);
        return true;
    }

    void __fastcall hkGeosetRenderPrep(void* component, void* edx)
    {
        g_origGeosetRenderPrep(component, edx);
        if (ApplyCustomizationGeometry(component))
            wxl_modern_m2::RefreshEquipmentGeometry(component);
    }

}

namespace wxl_modern_m2
{
    // Snapshots may arrive after the native geoset dirty bit has been consumed,
    // or before model/skin loading finishes. Reuse the exact post-native pass;
    // callers retry only until the saved recipe has reached a live instance.
    bool RefreshAppearanceGeometry(void* component)
    {
        const bool ready = ApplyCustomizationGeometry(component);
        // Saved recipes can arrive after the ordinary equipment render-prep pass.
        // Restore helmet/robe/cape visibility after customization selects its meshes.
        if (ready) RefreshEquipmentGeometry(component);
        return ready;
    }

    bool InstallCharacterGeosets()
    {
        if (!HookAttachByName("M2.CharGeosetRenderPrep", &hkGeosetRenderPrep, &g_origGeosetRenderPrep))
        {
            WLOG_WARN("char-geosets: the geoset decision could not be reached; modern race models keep"
                      " every high-numbered variant drawn at once");
            return false;
        }
        // The one that matters: without it no geoset past the 16-bit index line can be hidden at all,
        // and the decision below is deciding about pieces nobody can turn off.
        if (!HookAttachByName("M2.SetGeometryVisible", &hkSetGeometryVisible, &g_origSetGeometryVisible))
            WLOG_WARN("char-geosets: the visibility primitive could not be reached; on a modern race"
                      " model every variant past the 16-bit index line stays drawn");
        g_isolate = ConfigU32("WXL_M2_ISOLATE_GEOSET", kNoIsolation, 0, kNoIsolation);
        if (g_isolate != kNoIsolation)
        {
            const char* groupName = GeosetGroupName(g_isolate);
            if (groupName)
                WLOG_INFO("char-geosets: isolating geoset %u(%s) on modern race models; every other"
                          " piece is hidden", g_isolate, groupName);
            else
                WLOG_INFO("char-geosets: isolating geoset %u on modern race models; every other piece is"
                          " hidden", g_isolate);
        }
        else
            WLOG_INFO("char-geosets: geoset groups past the stock ceiling decided for modern race models");
        return true;
    }
}
