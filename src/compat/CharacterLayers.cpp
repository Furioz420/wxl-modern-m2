// What the modern tables put on the parts of a sheet the stock arrangement has no name for.
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
//   A modern layout is not the stock arrangement enlarged and nothing more. Measured on layout 104:
//   its first eight sections ARE that arrangement at twice the scale, to the pixel -- the arms, the
//   hands, the torso, the legs and the feet, in that order -- and then two more cover a further
//   1024x1024 beside it, which the stock composition has no region for and therefore never paints.
//   Those two are the scalp, upper and lower, which the stock arrangement keeps as small bands beside
//   the body and a high-resolution layout gives a square block of its own. Every reworked head lives
//   there. That is why a face composed entirely correctly still came out black: not painted wrong,
//   not painted at all.
//
//   What fills it is a different mechanism from the one that fills the stock half. The stock half is
//   regions, each with one source at a fixed place. This half is LAYERS: a choice names materials,
//   each material resolves to a file, and each carries a slot, a paint order, a blend mode and a mask
//   of which section types it covers. So the work here is to walk the ones belonging to THIS sheet's
//   slot, in order, and lay each into the sections its mask names.
//
//   A layer's file may be authored against ALL of them at once rather than against any one of them,
//   and that is the whole difficulty. On layout 104 a body file is 1024x512 against the 2048x1024
//   union of every section while a head file is 512x512 against the 1024x1024 block two sections
//   share: both exactly half scale, which only falls out if each is measured against the bounding box
//   of the sections its mask names. Measure a body against one section instead and its top-left corner
//   -- an upper arm -- is what gets blown up over the face, which is a face plainly made of texture
//   and plainly not a face.
//
//   The mask alone does not settle it, because a mask naming every section is also how a file that
//   covers only one is written when nothing restricts it. The file settles it: laid over the space it
//   was authored for it comes out at ONE scale, and at two different ones it was never that shape. So
//   the bounding box is offered and the rectangle is the answer when the shape refuses it.
//
//   The choices themselves are still not the character's own: the stock client keeps five legacy
//   bytes and the table that mapped them onto modern choices ships no rows. The first choice of each
//   option that contributes anything stands in, exactly as the geoset decision does, and the two are
//   resolved from ONE list so that an element waiting on a second choice can see it.

#include "DragonHornPolicy.hpp"
#include "../ExtensionApi.hpp"
#include "GilneanPreview.hpp"

#include "game/Binding.hpp"
#include "offsets/game/M2.hpp"
#include "wxl/AppearanceApi.h"
#include "wxl/FdidApi.h"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace
{
    namespace off = wxl::offsets::game::m2;

    const WXL_AppearanceApi* g_appearance = nullptr;

    const WXL_AppearanceApi* Appearance()
    {
        if (!g_appearance)
            g_appearance = static_cast<const WXL_AppearanceApi*>(
                wxl_modern_m2::g_api->GetInterface("wxl.appearance", WXL_APPEARANCE_API_VERSION));
        return g_appearance;
    }

    /// Turns a path into a texture cache entry, or finds the one already there. The pixels arrive
    /// asynchronously afterwards, exactly as they do for every source the stock composition asks for.
    using CreateTextureFn = uint32_t(__cdecl*)(const char* path);

    /// Gives back one of those references. The entry survives while any other holder remains.
    using ReleaseTextureFn = void(__cdecl*)(uint32_t source);

    /// Wide enough for every option a shipped model offers; a model that somehow exceeds it simply
    /// contributes its first choices, which is what the fallback would have done anyway.
    constexpr uint32_t kMaxChoices = 64;

    /**
     * @brief The choices this appearance stands on, gathered as ONE list.
     *
     * One list and not one per option, because an element can depend on a SECOND choice -- a hair
     * style lists one element per hair colour -- and the resolver can only honour that if it sees
     * every choice together.
     *
     * Taken outright rather than "the first that contributes", which is what the geoset decision can
     * afford: once dependencies are honoured, a choice judged ALONE contributes nothing at all, since
     * everything it carries is waiting on a partner that is not in a list of one. Judging that way
     * dropped every hair layer.
     */
    uint32_t GatherChoices(void* cmo, const WXL_AppearanceApi* tables, uint32_t chrModel, uint32_t race,
                           uint32_t sex, uint32_t* out)
    {
        // The geometry settled these already, and it has to be the same list: decided separately, both
        // answers are defensible and neither matches the other, which is a hair mesh wearing another
        // style's texture and a face that belongs to no head shown.
        if (const uint32_t settled = wxl_modern_m2::SettledChoicesFor(cmo, chrModel, out, kMaxChoices))
            return settled;

        uint32_t count = 0;
        const uint32_t options = tables->OptionCount(chrModel);

        for (uint32_t i = 0; i < options && count < kMaxChoices; ++i)
        {
            WXL_ChrOption option{};
            if (!tables->OptionAt(chrModel, i, &option)) continue;

            const uint32_t choices = tables->ChoiceCount(option.id);
            for (uint32_t c = 0; c < choices; ++c)
            {
                WXL_ChrChoice choice{};
                if (!tables->ChoiceAt(option.id, c, &choice)) continue;

                out[count++] = wxl_modern_m2::dragon::RequiredHorns(choice.id);
                break;
            }
        }
        return count;
    }

    /**
     * @brief How far across the sheet one layer's file reaches.
     *
     * A layer names section TYPES, not a rectangle, and its file covers all of them at once: measured
     * on layout 104, a body texture is 1024x512 against the 2048x1024 union of every section, and a
     * head texture is 512x512 against the 1024x1024 block sections 9 and 10 share. Both are exactly
     * half scale -- which only comes out if the span is the sections' bounding box, and comes out as
     * two different wrong answers if each section is scaled on its own.
     *
     * @return false when this layout has no section the mask names.
     */
    bool LayerSpan(const WXL_AppearanceApi* tables, uint32_t layoutId, uint32_t mask,
                   uint32_t stockRight, wxl_modern_m2::SheetRect& out)
    {
        const uint32_t sections = tables->SectionCount(layoutId);
        uint32_t left = 0, top = 0, right = 0, bottom = 0;
        bool any = false;

        for (uint32_t i = 0; i < sections; ++i)
        {
            WXL_TextureSection section{};
            if (!tables->SectionAt(layoutId, i, &section)) continue;
            if (section.sectionType >= 32 || !(mask & (1u << section.sectionType))) continue;

            if (!any) { left = section.x; top = section.y; right = 0; bottom = 0; any = true; }
            if (section.x < left) left = section.x;
            if (section.y < top) top = section.y;
            if (section.x + section.width > right) right = section.x + section.width;
            if (section.y + section.height > bottom) bottom = section.y + section.height;
        }
        if (!any) return false;

        // A base body material commonly declares every section because it is authored against the
        // WHOLE Retail atlas, including the head block to the right of the legacy regions. Clipping
        // that span to stockRight changes a 2:1 atlas into a square. PaintLayer then (correctly)
        // rejects that impossible scale and falls back to treating every arm/torso rectangle as an
        // independent copy of the source's top-left corner. Keep the full declared span even while
        // the caller is substituting only one stock region; dest is already a window onto it.
        (void)stockRight;
        if (right <= left || bottom <= top) return false;
        out = { left, top, right - left, bottom - top };
        return true;
    }

    /**
     * @brief The model texture slot the composed sheet IS.
     *
     * A layer names the slot it belongs to, and only this one is a sectioned composition: counted over
     * every layer row of every layout, type 1 has 588 of them and 491 name a real section, while types
     * 6, 8, 9, 10, 19, 20, 22 and 24 have not one section between them -- every single row of those
     * claims all sections at once, which is what a slot that is a WHOLE texture looks like when it is
     * written in a table shaped for sections.
     *
     * So they are not layers of this sheet at all. They are other textures the model samples in other
     * slots, and painting them here magnifies a 256x128 eye overlay eight times over the entire face.
     */
    constexpr uint32_t kSheetTextureType = 1;

    /**
     * @brief Paints the base skin into atlas cells which no named layout section covers.
     *
     * Retail character UVs can use deliberate gaps between the legacy body rectangles. Draenei
     * tails and facial appendages, for example, live in the lower-left gap of layouts 123/124.
     * Painting only SectionAt rectangles leaves the client's stale legacy source in those pixels.
     *
     * This runs only for the full-sheet base Blit pass. The grid is made from the section edges, so
     * every resulting cell is either wholly covered or wholly uncovered; only uncovered cells left
     * of stockRight are copied. Named body regions (and therefore equipment already composed over
     * them) are never touched.
     */
    bool AuthoredAppendageRect(uint32_t layoutId, uint32_t width, uint32_t height,
                               wxl_modern_m2::SheetRect& out)
    {
        // Verified section tables: 0..7 cover clothing; 9/10 cover the head.
        // Tauren 12 and Vulpera 13 are aggregate spans, not clothing rectangles.
        // Draenei 123/124 use the same gap for tail/tendril detail.
        // Pandaren 129/130 use it for paw pads and heel detail.
        // The uncovered lower-left cell holds authored appendage detail.
        if (width != 2048 || height != 1024 ||
            (layoutId != 113 && layoutId != 114 && layoutId != 145 && layoutId != 146 &&
             layoutId != 123 && layoutId != 124 &&
             layoutId != 129 && layoutId != 130))
            return false;
        out = {0, 640, 512, 384};
        return true;
    }

    void PaintBaseGaps(const WXL_AppearanceApi* tables, uint32_t layoutId, uint32_t source,
                       void* const* destLevels, uint32_t pitch,
                       const wxl_modern_m2::SheetRect& span,
                       uint32_t width, uint32_t height, uint32_t stockRight,
                       uint32_t& painted, uint32_t& waiting, uint32_t& reading)
    {
        if (!tables || !source || !destLevels || !stockRight || !height) return;
        if (stockRight > width) stockRight = width;

        std::vector<uint32_t> xs{ 0, stockRight };
        std::vector<uint32_t> ys{ 0, height };
        const uint32_t sectionCount = tables->SectionCount(layoutId);
        for (uint32_t i = 0; i < sectionCount; ++i)
        {
            WXL_TextureSection section{};
            if (!tables->SectionAt(layoutId, i, &section) || section.x >= stockRight) continue;
            const uint32_t right = std::min(stockRight, section.x + section.width);
            const uint32_t bottom = std::min(height, section.y + section.height);
            xs.push_back(section.x);
            xs.push_back(right);
            ys.push_back(section.y);
            ys.push_back(bottom);
        }
        std::sort(xs.begin(), xs.end());
        xs.erase(std::unique(xs.begin(), xs.end()), xs.end());
        std::sort(ys.begin(), ys.end());
        ys.erase(std::unique(ys.begin(), ys.end()), ys.end());

        for (size_t yi = 1; yi < ys.size(); ++yi)
        {
            for (size_t xi = 1; xi < xs.size(); ++xi)
            {
                const wxl_modern_m2::SheetRect cell{
                    xs[xi - 1], ys[yi - 1],
                    xs[xi] - xs[xi - 1], ys[yi] - ys[yi - 1]
                };
                if (!cell.width || !cell.height) continue;

                bool covered = false;
                for (uint32_t s = 0; s < sectionCount && !covered; ++s)
                {
                    WXL_TextureSection section{};
                    if (!tables->SectionAt(layoutId, s, &section)) continue;
                    covered = section.x <= cell.x && section.y <= cell.y &&
                        section.x + section.width >= cell.x + cell.width &&
                        section.y + section.height >= cell.y + cell.height;
                }
                if (covered) continue;

                const wxl_modern_m2::LayerPaint did =
                    wxl_modern_m2::PaintLayer(source, destLevels, pitch, 1, span, cell);
                if (did == wxl_modern_m2::LayerPaint::Painted) ++painted; else ++waiting;
                if (did == wxl_modern_m2::LayerPaint::Reading) ++reading;
            }
        }
    }

}

namespace wxl_modern_m2
{
    uint32_t PaintModernLayers(void* cmo, uint32_t chrRaceId, uint32_t sex, uint32_t layoutId,
                               void* const* destLevels, uint32_t width, uint32_t height,
                               uint32_t stockRight, uint32_t sectionFilter, uint32_t& outReading)
    {
        outReading = 0;
        const WXL_AppearanceApi* tables = Appearance();
        const WXL_FdidApi* files = Fdid();
        if (!tables || !files || !destLevels || !layoutId) return 0;

        uint32_t retailRace = wxl_modern_m2::RetailCharacterRace(chrRaceId);
        // Use the same form identity as geometry and sheet layout, including world spells.
        const bool alternate = AlternateFormIdentity(cmo, retailRace, sex);
        if (!RetailCharacterCanaryAllows(alternate ? (retailRace == 23 ? 12 : retailRace == 70 ? 17 : 30) : chrRaceId, sex)) return 0;
        const uint32_t chrModel = tables->ChrModelForRace(retailRace, sex);
        if (!chrModel) return 0;

        uint32_t choices[kMaxChoices];
        const uint32_t choiceCount = GatherChoices(cmo, tables, chrModel, chrRaceId, sex, choices);
        if (!choiceCount) return 0;

        WXL_Recipe recipe{};
        if (!tables->BuildForCharacter(retailRace, sex, choices, choiceCount, &recipe)) return 0;

        const auto create = wxl::game::Native<CreateTextureFn>(off::kCharCreateTextureFromPath);
        const auto release = wxl::game::Native<ReleaseTextureFn>(off::kTextureCacheRelease);
        const uint32_t pitch = width * 4;
        uint32_t painted = 0, waiting = 0;

        // In the order the tables state, because layers are not commutative: a later one covers.
        for (uint32_t i = 0; i < recipe.layerCount; ++i)
        {
            const WXL_TextureLayer& layer = recipe.layers[i];
            if (layer.textureType != kSheetTextureType) continue;
            const char* const path =
                files->ResolveMaterialTexture(layer.materialResourceId, layer.textureType);
            if (!path || !*path) continue;

            // Naming the file takes a reference on its cache entry every time. PaintLayer asks the
            // cache for its description with wait=1, so the source is finished synchronously before
            // the call returns and this reference can always be released at the end of the layer.
            // Bounded, and paired with the line the paint itself emits: which layer a source belongs
            // to is the difference between a file laid in the wrong place and the wrong file entirely.
            static uint32_t announcementsLeft = 12;
            if (announcementsLeft)
            {
                --announcementsLeft;
                WLOG_INFO("char-layers: order %u, type %u, blend %u, sections %#010x -> %s",
                          layer.layer, layer.textureType, layer.blendMode, layer.sectionMask, path);
            }

            const uint32_t source = create(path);
            if (!source) continue;
            SheetRect span{};
            if (!LayerSpan(tables, layoutId, layer.sectionMask,
                           sectionFilter == kEverySection ? 0 : stockRight, span))
            {
                release(source);
                continue;
            }

            const uint32_t sections = tables->SectionCount(layoutId);
            uint32_t doneX = width, doneY = height, doneW = 0, doneH = 0;

            for (uint32_t i = 0; i < sections; ++i)
            {
                WXL_TextureSection section{};
                if (!tables->SectionAt(layoutId, i, &section)) continue;
                if (section.sectionType >= 32) continue;
                if (!(layer.sectionMask & (1u << section.sectionType))) continue;
                if (sectionFilter != kEverySection)
                {
                    // Asked for by type: the caller already decided this section is ours, in place
                    // of a source the client would otherwise have painted there itself.
                    if (section.sectionType != sectionFilter) continue;
                }
                // A layer may name every section at once. This pass runs after the client has filled
                // the sheet, worn items included, so anything the stock arrangement reaches is left
                // alone here whether or not it was composed well: painting over it now would take the
                // armour with it. Those sections are substituted at the point they are painted
                // instead, which is early enough for what is worn to land back on top.
                else if (section.x < stockRight) continue;
                if (section.x + section.width > width || section.y + section.height > height) continue;
                // Two section types can share one rectangle -- a reworked head answers to both of the
                // two the stock arrangement kept apart -- and painting it twice lays the layer over
                // itself, which its blend would compound.
                if (section.x == doneX && section.y == doneY
                    && section.width == doneW && section.height == doneH) continue;
                doneX = section.x; doneY = section.y;
                doneW = section.width; doneH = section.height;

                uint32_t blend = layer.blendMode;
                if (retailRace == 26 && sex == 0 && layoutId == 129 &&
                    layer.sectionMask == (1u << 3) && blend == 15)
                    blend = wxl_modern_m2::kLayerBlendFeatherBottom;
                const LayerPaint did = PaintLayer(source, destLevels, pitch, blend, span,
                                                  { section.x, section.y,
                                                    section.width, section.height });
                if (did == LayerPaint::Painted) ++painted; else ++waiting;
                if (did == LayerPaint::Reading) ++outReading;
            }

            // The base skin is authored against the whole atlas, including cells which the section
            // table intentionally leaves unnamed for appendage UVs. Fill only those holes after the
            // client's named-region composition; later layers and equipment remain untouched.
            if (sectionFilter == kEverySection && layer.layer == 0 &&
                layer.blendMode == 1 && layer.sectionMask == 0xFFFFFFFFu)
            {
                SheetRect appendage{};
                if (AuthoredAppendageRect(layoutId, width, height, appendage))
                {
                    const LayerPaint did = PaintLayer(source, destLevels, pitch, 1, span, appendage);
                    static uint32_t appendageDiagnostics = 0;
                    if (appendageDiagnostics++ < 16)
                        WLOG_INFO("appendage-atlas: layout=%u source=%s result=%u span=%u,%u,%u,%u",
                                  layoutId, path, static_cast<uint32_t>(did),
                                  span.x, span.y, span.width, span.height);
                    if (did == LayerPaint::Painted) ++painted; else ++waiting;
                    if (did == LayerPaint::Reading) ++outReading;
                }
                else
                    PaintBaseGaps(tables, layoutId, source, destLevels, pitch, span,
                                  width, height, stockRight, painted, waiting, outReading);
            }

            // PaintLayer's blocking description request has completed any source read. Retaining this
            // handle beyond the paint is both unnecessary and unsafe: the native cache can recycle
            // the entry before process shutdown, leaving a stale pointer for a later release.
            release(source);
        }

        // Reported while sources are still arriving, not once: the count falling to nothing across
        // rebuilds is what says the reads land, and a single line could never show that.
        static uint32_t left = 8;
        if (left && (painted || waiting))
        {
            if (!waiting) left = 0; else --left;
            WLOG_INFO("char-layers: layout %u took %u layer(s) over %u section paint(s), %u source(s)"
                      " not loaded yet", layoutId, recipe.layerCount, painted, waiting);
        }
        return painted;
    }
}
