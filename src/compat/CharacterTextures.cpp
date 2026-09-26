// Composing the character sheet from source textures the stock copy cannot read.
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
//   A character's skin, face and hair are not textures the renderer samples directly. They are pasted
//   into one composited sheet on the CPU, region by region, and the sheet is what the model samples.
//   That composition is written against palettised sources and only those: both of its copies resolve
//   the source's palette before anything else and abandon the source when there is none, without ever
//   reaching the format dispatch that follows. A source in any other encoding therefore contributes
//   nothing, silently, and its regions of the sheet keep the colour the sheet was cleared to.
//
//   Reworked models ship their skins as compressed blocks, which have no palette by construction. So
//   the copy has to be ours for those, and only for those: when the source is palettised the stock
//   copy is correct and keeps the work. What follows reproduces its geometry exactly -- the same
//   level walk, the same halving per level, the same doubling with its seam averaging, the same
//   destination layout of one pointer per sheet level -- and changes only where the pixels are read
//   from. Reproducing it rather than approximating it is what keeps a region we paint aligned with a
//   region the stock path paints, since a character mixes both.

#include "../ExtensionApi.hpp"

#include "game/Binding.hpp"
#include "offsets/game/M2.hpp"

#include <cstdint>
#include "CharacterCopyCache.hpp"

namespace
{
    namespace off = wxl::offsets::game::m2;

    /// Fields of the container file the texture cache keeps verbatim.
    constexpr size_t kOffImageEncoding = 0x08;

    /// Where a palettised container keeps its palette, and how a container states its encoding.
    constexpr size_t kOffImagePalette = 0x94;

    enum : uint8_t
    {
        kEncodingPalettised = 1,
        kEncodingBlocks     = 2,
        kEncodingDirect     = 3,
    };

    /// The six bytes both copies receive as their source description.
    struct SourceDesc
    {
        uint16_t width;
        uint16_t height;
        uint8_t  levelCount;
        uint8_t  alphaBits;
    };

    using GetLevelFn = const uint8_t*(__cdecl*)(uint32_t source, uint32_t level);

    uint32_t AtLeastOne(uint32_t value) { return value ? value : 1u; }

    uint32_t Smaller(uint32_t a, uint32_t b) { return a < b ? a : b; }

    // -- reading one sample out of a source ------------------------------------------------------

    struct Texel
    {
        uint8_t b, g, r, a;
    };

    uint32_t ReadLE16(const uint8_t* at) { return uint32_t(at[0]) | (uint32_t(at[1]) << 8); }

    uint32_t ReadLE32(const uint8_t* at)
    {
        return ReadLE16(at) | (ReadLE16(at + 2) << 16);
    }

    /// Widens a channel to eight bits the way sampling hardware does, by repeating its high bits into
    /// the low ones. Shifting alone would leave full scale one step short of white and tint every
    /// bright pixel.
    uint8_t Widen(uint32_t value, uint32_t bits)
    {
        return uint8_t((value << (8 - bits)) | (value >> (2 * bits - 8)));
    }

    Texel FromPacked565(uint32_t packed)
    {
        return { Widen(packed & 0x1F, 5), Widen((packed >> 5) & 0x3F, 6),
                 Widen((packed >> 11) & 0x1F, 5), 0xFF };
    }

    uint8_t Weighted(uint32_t a, uint32_t b, uint32_t weightA, uint32_t weightB)
    {
        return uint8_t((a * weightA + b * weightB) / (weightA + weightB));
    }

    /// The four colours a block's two-bit selectors choose between. A block that lists its endpoints
    /// in ascending order spends its last slot on transparency instead of a fourth colour, but only
    /// where the container has no alpha of its own -- with one, both endpoint orders mean four
    /// colours and the ascending case would otherwise punch holes through opaque skin.
    void BuildRamp(const uint8_t* colours, bool endpointOrderCarriesAlpha, Texel ramp[4])
    {
        const uint32_t first = ReadLE16(colours);
        const uint32_t second = ReadLE16(colours + 2);
        ramp[0] = FromPacked565(first);
        ramp[1] = FromPacked565(second);

        if (first > second || !endpointOrderCarriesAlpha)
        {
            ramp[2] = { Weighted(ramp[0].b, ramp[1].b, 2, 1), Weighted(ramp[0].g, ramp[1].g, 2, 1),
                        Weighted(ramp[0].r, ramp[1].r, 2, 1), 0xFF };
            ramp[3] = { Weighted(ramp[0].b, ramp[1].b, 1, 2), Weighted(ramp[0].g, ramp[1].g, 1, 2),
                        Weighted(ramp[0].r, ramp[1].r, 1, 2), 0xFF };
        }
        else
        {
            ramp[2] = { Weighted(ramp[0].b, ramp[1].b, 1, 1), Weighted(ramp[0].g, ramp[1].g, 1, 1),
                        Weighted(ramp[0].r, ramp[1].r, 1, 1), 0xFF };
            ramp[3] = { 0, 0, 0, 0 };
        }
    }

    /// The alpha a block states for one of its texels, where it states them one nibble each.
    uint8_t TabulatedAlpha(const uint8_t* block, uint32_t texel)
    {
        const uint32_t nibble = (block[texel >> 1] >> ((texel & 1) * 4)) & 0xF;
        return uint8_t(nibble * 0x11);
    }

    /// The alpha a block states for one of its texels, where it states two ends and a three-bit
    /// position between them. Descending ends spend the last two positions on the extremes instead
    /// of on interpolation.
    uint8_t InterpolatedAlpha(const uint8_t* block, uint32_t texel)
    {
        const uint32_t low = block[0];
        const uint32_t high = block[1];
        const uint32_t bit = texel * 3;
        const uint32_t packed = ReadLE32(block + 2 + (bit >> 3)) >> (bit & 7);
        const uint32_t position = packed & 7;

        if (position == 0) return uint8_t(low);
        if (position == 1) return uint8_t(high);
        if (low > high) return Weighted(low, high, 8 - position, position - 1);
        if (position < 6) return Weighted(low, high, 6 - position, position - 1);
        return position == 6 ? 0x00 : 0xFF;
    }

    Texel SampleBlocks(const uint8_t* level, uint32_t blocksPerRow, uint8_t alphaBits,
                       uint32_t x, uint32_t y)
    {
        // Four bits of alpha and above are stated per texel in a block of their own, ahead of the
        // colours. One bit and below leaves the colour block to carry it in its endpoint order.
        const bool alphaHasItsOwnBlock = alphaBits > 1;
        const size_t stride = alphaHasItsOwnBlock ? 16 : 8;
        const uint8_t* const block = level + ((y >> 2) * blocksPerRow + (x >> 2)) * stride;
        const uint8_t* const colours = alphaHasItsOwnBlock ? block + 8 : block;

        Texel ramp[4];
        BuildRamp(colours, !alphaHasItsOwnBlock, ramp);

        const uint32_t texel = (y & 3) * 4 + (x & 3);
        Texel out = ramp[(ReadLE32(colours + 4) >> (texel * 2)) & 3];

        if (alphaBits == 4) out.a = TabulatedAlpha(block, texel);
        else if (alphaBits == 8) out.a = InterpolatedAlpha(block, texel);
        return out;
    }

    /**
     * @brief The alpha a palettised container states for one texel.
     *
     * Held in a plane of its own that FOLLOWS the whole index plane, packed to however many bits the
     * container declares. Ignoring it and calling every texel opaque is not a subtle loss: an overlay
     * is mostly transparent by nature, so its whole rectangle lands as a solid patch over whatever it
     * was meant to sit on.
     */
    uint8_t PlanarAlpha(const uint8_t* level, uint32_t levelWidth, uint32_t levelHeight,
                        uint8_t alphaBits, uint32_t x, uint32_t y)
    {
        const uint8_t* const plane = level + size_t(levelWidth) * levelHeight;
        const size_t texel = y * size_t(levelWidth) + x;

        if (alphaBits == 8) return plane[texel];
        if (alphaBits == 4) return uint8_t(((plane[texel >> 1] >> ((texel & 1) * 4)) & 0xF) * 0x11);
        if (alphaBits == 1) return (plane[texel >> 3] >> (texel & 7)) & 1 ? 0xFF : 0x00;
        return 0xFF;
    }

    Texel Sample(const uint8_t* level, uint8_t encoding, const uint8_t* palette,
                 uint32_t levelWidth, uint32_t levelHeight, uint8_t alphaBits,
                 uint32_t x, uint32_t y)
    {
        if (encoding == kEncodingPalettised)
        {
            const uint8_t* const entry = palette + size_t(level[y * size_t(levelWidth) + x]) * 4;
            return { entry[0], entry[1], entry[2],
                     alphaBits ? PlanarAlpha(level, levelWidth, levelHeight, alphaBits, x, y)
                               : uint8_t(0xFF) };
        }
        if (encoding == kEncodingDirect)
        {
            const uint8_t* const at = level + (y * size_t(levelWidth) + x) * 4;
            return { at[0], at[1], at[2], at[3] };
        }
        return SampleBlocks(level, AtLeastOne((levelWidth + 3) / 4), alphaBits, x, y);
    }

    // Bilinear magnification samples the same 4x4 compressed block repeatedly.
    // Decode those 16 texels once per nearby block instead of rebuilding the
    // endpoint ramps for each of four samples at every output pixel. Four slots
    // retain the two neighboring block columns in two neighboring block rows.
    // This cache belongs to one copy of one mip: no source pointer or decoded
    // color survives a layer, texture reload, form switch, or frame.
    struct SourceSampler
    {
        const uint8_t* level;
        uint8_t encoding;
        const uint8_t* palette;
        uint32_t width, height;
        uint8_t alphaBits;

        struct Block
        {
            const uint8_t* address = nullptr;
            Texel texels[16];
        } blocks[4];

        SourceSampler(const uint8_t* pixels, uint8_t format, const uint8_t* colors,
                      uint32_t w, uint32_t h, uint8_t alpha)
            : level(pixels), encoding(format), palette(colors), width(w), height(h), alphaBits(alpha) {}

        Texel At(uint32_t x, uint32_t y)
        {
            if (encoding != kEncodingBlocks)
                return Sample(level, encoding, palette, width, height, alphaBits, x, y);

            const bool separateAlpha = alphaBits > 1;
            const uint32_t bx = x >> 2, by = y >> 2;
            const uint8_t* address = level + (by * AtLeastOne((width + 3) / 4) + bx)
                                           * (separateAlpha ? 16u : 8u);
            Block& cached = blocks[((by & 1) << 1) | (bx & 1)];
            if (cached.address != address)
            {
                const uint8_t* colors = address + (separateAlpha ? 8 : 0);
                Texel ramp[4];
                BuildRamp(colors, !separateAlpha, ramp);
                const uint32_t selectors = ReadLE32(colors + 4);
                uint8_t alphaRamp[8];
                if (alphaBits == 8)
                {
                    alphaRamp[0] = address[0];
                    alphaRamp[1] = address[1];
                    for (uint32_t i = 2; i < 8; ++i)
                        alphaRamp[i] = address[0] > address[1]
                            ? Weighted(address[0], address[1], 8 - i, i - 1)
                            : i < 6 ? Weighted(address[0], address[1], 6 - i, i - 1)
                                    : i == 6 ? 0 : 255;
                }
                for (uint32_t i = 0; i < 16; ++i)
                {
                    Texel value = ramp[(selectors >> (i * 2)) & 3];
                    if (alphaBits == 4) value.a = TabulatedAlpha(address, i);
                    else if (alphaBits == 8)
                    {
                        const uint32_t bit = i * 3;
                        const uint32_t offset = 2 + (bit >> 3);
                        // At most two bytes contain a selector. Do not read a
                        // 32-bit word beyond the six-byte alpha selector field.
                        uint32_t packed = address[offset];
                        if ((bit & 7) > 5) packed |= uint32_t(address[offset + 1]) << 8;
                        value.a = alphaRamp[(packed >> (bit & 7)) & 7];
                    }
                    cached.texels[i] = value;
                }
                cached.address = address; // Publish only after the whole decode succeeds.
            }
            return cached.texels[(y & 3) * 4 + (x & 3)];
        }
    };

    /// One step of `steps` from `a` towards `b`. At two steps this is the plain mean, which is what
    /// the doubling it generalises used.
    Texel Mixed(Texel a, Texel b, uint32_t step, uint32_t steps)
    {
        if (step == 0 || steps < 2) return a;
        const uint32_t to = step, from = steps - step;
        return { uint8_t((a.b * from + b.b * to) / steps), uint8_t((a.g * from + b.g * to) / steps),
                 uint8_t((a.r * from + b.r * to) / steps), uint8_t((a.a * from + b.a * to) / steps) };
    }

    // -- writing it into the sheet ---------------------------------------------------------------

    /**
     * @brief How a layer combines with what the sheet already holds, as the tables name it.
     *
     * The layer row carries this per layer, and the values are not ours to choose. On the layouts
     * measured, a base skin arrives as Blit, most section-scoped pieces as InferAlpha, a skin tone
     * over the whole sheet as Overlay, and an eye-glow face overlay as Screen. Treating them all as
     * "opaque overwrites, translucent blends" gets the first two right by coincidence and the last
     * two wrong in a way that reads as a lighting fault rather than a compositing one.
     */
    enum : uint32_t
    {
        kBlendBlit          = 1,   ///< straight copy; the source's own alpha does not hold it back
        kBlendMultiply      = 4,   ///< white preserves the destination; coloured ink multiplies it
        kBlendOverlay       = 6,
        kBlendScreen        = 7,
        kBlendAlphaStraight = 9,
        kBlendInferAlpha    = 15,  ///< alpha blend, alpha taken from the source; none means opaque

        /// Not a value the tables use: what a copy with no layer row behind it does. A source that
        /// declares no alpha overwrites outright rather than being weighed by the alpha its decoding
        /// happened to produce, which for compressed blocks is a real value on some texels and would
        /// turn every one of them into whatever the sheet held underneath.
        kBlendAsAuthored    = 0xFFFFFFFFu,
    };

    uint8_t OverlayChannel(uint32_t base, uint32_t over)
    {
        return over < 128 ? uint8_t((2 * base * over) / 255)
                          : uint8_t(255 - (2 * (255 - base) * (255 - over)) / 255);
    }

    uint8_t ScreenChannel(uint32_t base, uint32_t over)
    {
        return uint8_t(255 - ((255 - base) * (255 - over)) / 255);
    }

    /// Weighs a combined channel against what was there, by how present the source is. Blit ignores
    /// this by construction; every other mode is only as strong as the source's own alpha.
    uint8_t Weigh(uint32_t had, uint32_t got, uint32_t alpha)
    {
        return uint8_t((got * alpha + had * (255u - alpha)) / 255u);
    }

    /// Lays one sample over what the sheet already holds. The sheet's own alpha is left opaque
    /// whatever the mode: it is a composition target, not a layer.
    void Lay(uint32_t* destination, Texel source, uint32_t blend, uint8_t alphaBits)
    {
        if (blend == kBlendAsAuthored && alphaBits == 0)
        {
            *destination = 0xFF000000u | (uint32_t(source.r) << 16) | (uint32_t(source.g) << 8)
                         | source.b;
            return;
        }

        const uint32_t had = *destination;
        const uint32_t hr = (had >> 16) & 0xFF, hg = (had >> 8) & 0xFF, hb = had & 0xFF;

        uint32_t r = source.r, g = source.g, b = source.b;
        if (blend == kBlendMultiply)
        {
            r = (hr * source.r) / 255u;
            g = (hg * source.g) / 255u;
            b = (hb * source.b) / 255u;
        }
        else if (blend == kBlendOverlay)
        {
            r = OverlayChannel(hr, source.r);
            g = OverlayChannel(hg, source.g);
            b = OverlayChannel(hb, source.b);
        }
        else if (blend == kBlendScreen)
        {
            r = ScreenChannel(hr, source.r);
            g = ScreenChannel(hg, source.g);
            b = ScreenChannel(hb, source.b);
        }

        if (blend != kBlendBlit)
        {
            r = Weigh(hr, r, source.a);
            g = Weigh(hg, g, source.a);
            b = Weigh(hb, b, source.a);
        }
        *destination = 0xFF000000u | (r << 16) | (g << 8) | b;
    }

    /// One rectangle of one source, on its way into the sheet. Carried by value through the level
    /// walk, which halves every one of these as it descends.
    struct Blit
    {
        uint32_t source;
        void* const* destLevels;
        const SourceDesc* desc;
        const uint8_t* palette;
        uint8_t encoding;
        /// The mode the layer row names, or kBlendAsAuthored for a copy that has no row behind it.
        uint32_t blend;
        uint32_t destX, destY;
        uint32_t srcX, srcY;
        uint32_t width, height;
        uint32_t pitch;
        // Optional owned source snapshot for one cached copy. Never exported.
        const uint8_t* const* capturedLevels = nullptr;
    };

    Texel FeatherLowerEdge(const Blit& blit, Texel sample, uint32_t row)
    {
        if (blit.blend != wxl_modern_m2::kLayerBlendFeatherBottom || blit.height < 4)
            return sample;
        const uint32_t feather = blit.height / 4;
        if (row < blit.height - feather) return sample;
        const uint32_t remaining = blit.height - 1 - row;
        sample.a = static_cast<uint8_t>((uint32_t(sample.a) * remaining) / feather);
        return sample;
    }

    uint8_t* DestRow(const Blit& blit, void* levelBase, uint32_t row)
    {
        return static_cast<uint8_t*>(levelBase) + (blit.destY + row) * size_t(blit.pitch)
             + blit.destX * 4;
    }

    /// Walks the source's levels one for one into the sheet's, from `firstLevel` down. Every extent
    /// halves per level, so the same rectangle keeps naming the same part of the picture as both
    /// sides shrink; `destBias` is what turns a source level into the sheet level it lands on.
    void CopyLevels(Blit blit, uint32_t firstLevel, int32_t destBias)
    {
        const auto getLevel = wxl::game::Native<GetLevelFn>(off::kTextureCacheGetLevel);

        for (uint32_t level = firstLevel; level < blit.desc->levelCount; ++level)
        {
            const uint8_t* const pixels = blit.capturedLevels ? blit.capturedLevels[level]
                                                            : getLevel(blit.source, level);
            void* const base = blit.destLevels[int32_t(level) + destBias];
            if (pixels && base)
            {
                const uint32_t levelWidth = AtLeastOne(uint32_t(blit.desc->width) >> level);
                const uint32_t levelHeight = AtLeastOne(uint32_t(blit.desc->height) >> level);
                SourceSampler sampler(pixels, blit.encoding, blit.palette, levelWidth,
                                      levelHeight, blit.desc->alphaBits);
                // Clamped for the same reason the magnifying copy is: a source and the rectangle it
                // is asked to fill are paired by the tables and need not agree in size.
                const uint32_t lastX = levelWidth > blit.srcX ? levelWidth - blit.srcX - 1 : 0;
                const uint32_t lastY = levelHeight > blit.srcY ? levelHeight - blit.srcY - 1 : 0;
                for (uint32_t y = 0; y < blit.height; ++y)
                {
                    auto* const out = reinterpret_cast<uint32_t*>(DestRow(blit, base, y));
                    for (uint32_t x = 0; x < blit.width; ++x)
                        Lay(out + x, FeatherLowerEdge(blit,
                            sampler.At(blit.srcX + Smaller(x, lastX),
                                       blit.srcY + Smaller(y, lastY)), y),
                            blit.blend, blit.desc->alphaBits);
                }
            }

            blit.width = AtLeastOne(blit.width >> 1);
            blit.height = AtLeastOne(blit.height >> 1);
            blit.destX >>= 1;
            blit.destY >>= 1;
            blit.srcX >>= 1;
            blit.srcY >>= 1;
            blit.pitch >>= 1;
        }
    }

    /**
     * @brief The sheet's top level from a source authored at a whole fraction of its scale.
     *
     * The stock copy knows exactly two scales, one to one and one to two, because the arrangement it
     * was written for only ever asked for those. A layout that states a larger sheet enlarges every
     * region by the same factor, and a source that was already at half scale then needs four -- which
     * that copy answers by reading past the end of every row.
     *
     * Weighted the way the doubling was, so a factor of two reproduces it exactly and the regions
     * this takes over stay indistinguishable from the ones it does not: each destination pixel falls
     * between two source pixels and is mixed by how far along it sits, rather than snapping to the
     * nearer, which would show as blocks against a neighbouring region that was smoothed.
     */
    void CopyMagnified(const Blit& blit, uint32_t factor, uint32_t level, int32_t destBias)
    {
        const uint8_t* const pixels = blit.capturedLevels ? blit.capturedLevels[level] :
            wxl::game::Native<GetLevelFn>(off::kTextureCacheGetLevel)(blit.source, level);
        void* const base = blit.destLevels[destBias];
        if (!pixels || !base || !factor) return;

        const uint32_t levelWidth = AtLeastOne(uint32_t(blit.desc->width) >> level);
        const uint32_t levelHeight = AtLeastOne(uint32_t(blit.desc->height) >> level);
        if (blit.srcX >= levelWidth || blit.srcY >= levelHeight) return;
        // Bounded by the SOURCE and not only by what the destination asks for. A source that does not
        // cover its destination at this factor is ordinary -- the tables pair a section with whatever
        // file a material names, and the two need not agree -- and reading on regardless walks off the
        // end of the image by however much they differ.
        const uint32_t lastX = Smaller(AtLeastOne(blit.width / factor), levelWidth - blit.srcX) - 1;
        const uint32_t lastY = Smaller(AtLeastOne(blit.height / factor), levelHeight - blit.srcY) - 1;

        SourceSampler sampler(pixels, blit.encoding, blit.palette, levelWidth,
                              levelHeight, blit.desc->alphaBits);
        const auto at = [&](uint32_t sx, uint32_t sy) { return sampler.At(sx, sy); };

        for (uint32_t y = 0; y < blit.height; ++y)
        {
            const uint32_t nearY = blit.srcY + Smaller(y / factor, lastY);
            const uint32_t farY = blit.srcY + Smaller(y / factor + 1, lastY);
            const uint32_t downY = y % factor;
            auto* const out = reinterpret_cast<uint32_t*>(DestRow(blit, base, y));

            for (uint32_t x = 0; x < blit.width; ++x)
            {
                const uint32_t nearX = blit.srcX + Smaller(x / factor, lastX);
                const uint32_t farX = blit.srcX + Smaller(x / factor + 1, lastX);
                const uint32_t alongX = x % factor;

                const Texel top = Mixed(at(nearX, nearY), at(farX, nearY), alongX, factor);
                const Texel bottom = Mixed(at(nearX, farY), at(farX, farY), alongX, factor);
                Lay(out + x, FeatherLowerEdge(blit, Mixed(top, bottom, downY, factor), y),
                    blit.blend, blit.desc->alphaBits);
            }
        }
    }

    /**
     * @brief The whole chain for one region: the top level at its magnification, then every level
     *        below it, each at half the one above until the two scales meet and the rest copies one
     *        for one.
     *
     * One routine for both entry points because the difference between them is only where the chain
     * starts: a source authored below the sheet's scale is magnified into the top levels and lands on
     * its own scale further down, and a source already at that scale simply starts there.
     */
    void CopyChain(Blit blit, uint32_t factor, uint32_t firstLevel)
    {
        int32_t landed = 0;
        for (uint32_t left = factor; left > 1; left >>= 1)
        {
            CopyMagnified(blit, left, firstLevel, landed);
            ++landed;
            blit.width = AtLeastOne(blit.width >> 1);
            blit.height = AtLeastOne(blit.height >> 1);
            blit.destX >>= 1;
            blit.destY >>= 1;
            blit.pitch >>= 1;
        }
        CopyLevels(blit, firstLevel, landed - int32_t(firstLevel));
    }

    bool CopyChainUncachedGuarded(Blit blit, uint32_t factor, uint32_t firstLevel) noexcept
    {
        // Texture cache entries are completed and evicted by the client's resource queue. Retail
        // composition can observe a valid description immediately before an old level pointer is
        // retired; dereferencing that DXT block was the Draenei Error #132 at BuildRamp. Decline this
        // one paint and let the next sheet rebuild retry it instead of taking down the Glue process.
        __try
        {
            CopyChain(blit, factor, firstLevel);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // Cache completed copy results by content, including the pixels beneath a
    // blended layer. Neither a component pointer nor a choice ID is sufficient:
    // reused addresses, changed gear, and another form can otherwise share a key.
    struct CopyCapture
    {
        SourceDesc desc{};
        std::array<wxl_copy_cache::Bytes,13> source;
        std::array<const uint8_t*,13> levels{};
        std::array<uint8_t,1024> palette{};
        struct Target
        {
            void* base;
            uint32_t x,y,width,height,pitch;
            wxl_copy_cache::Bytes before;
        };
        std::vector<Target> targets;
        size_t resultBytes=0;
        wxl_copy_cache::Key key{};
    };

    const uint8_t* CacheLevel(uint32_t source,uint32_t level) noexcept
    {
        __try { return wxl::game::Native<GetLevelFn>(off::kTextureCacheGetLevel)(source,level); }
        __except(EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }

    bool CaptureCopy(Blit& blit,uint32_t factor,uint32_t firstLevel,CopyCapture& capture)
    {
        using namespace wxl_copy_cache;
        if(!factor || factor>16 || (factor&(factor-1)) ||
           !Copy(&capture.desc,blit.desc,sizeof(capture.desc)))return false;
        auto& desc=capture.desc;
        if(!desc.width || !desc.height || desc.width>4096 || desc.height>4096 ||
           !desc.levelCount || desc.levelCount>13 || firstLevel>=desc.levelCount)return false;
        blit.desc=&desc;
        Digest digest;
        // Increment when compositing, interpolation, blending or decoding changes.
        const uint32_t metadata[]={0x43435002,desc.width,desc.height,desc.levelCount,desc.alphaBits,
            blit.encoding,blit.blend,blit.destX,blit.destY,blit.srcX,blit.srcY,
            blit.width,blit.height,blit.pitch,factor,firstLevel};
        if(!digest.Add(metadata,sizeof(metadata)))return false;
        if(blit.encoding==kEncodingPalettised){
            if(!blit.palette || !Copy(capture.palette.data(),blit.palette,capture.palette.size()) ||
               !digest.Add(capture.palette.data(),capture.palette.size()))return false;
            blit.palette=capture.palette.data();
        }
        size_t sourceBytes=0;
        for(uint32_t level=firstLevel;level<desc.levelCount;++level){
            uint32_t w=AtLeastOne(uint32_t(desc.width)>>level),h=AtLeastOne(uint32_t(desc.height)>>level);
            uint64_t size=0;
            if(blit.encoding==kEncodingBlocks)size=uint64_t((w+3)/4)*((h+3)/4)*(desc.alphaBits>1?16:8);
            else if(blit.encoding==kEncodingDirect)size=uint64_t(w)*h*4;
            else if(blit.encoding==kEncodingPalettised && (desc.alphaBits==0 || desc.alphaBits==1 || desc.alphaBits==4 || desc.alphaBits==8))
                size=uint64_t(w)*h+(uint64_t(w)*h*desc.alphaBits+7)/8;
            else return false;
            if(!size || size>kMaxEntry || sourceBytes+size>kMaxEntry)return false;
            const uint8_t* pixels=CacheLevel(blit.source,level);
            if(!pixels)return false; // Never cache a partial/missing source chain.
            auto& owned=capture.source[level];owned.resize(size_t(size));
            if(!Copy(owned.data(),pixels,owned.size()) || !digest.Add(owned.data(),owned.size()))return false;
            capture.levels[level]=owned.data();sourceBytes+=size_t(size);
        }
        blit.capturedLevels=capture.levels.data();

        const auto target=[&](const Blit& region,uint32_t destinationLevel)->bool {
            if(destinationLevel>=13)return false;
            void* base=nullptr;
            if(!Copy(&base,blit.destLevels+destinationLevel,sizeof(base)) || !base)return false;
            const uint64_t rowBytes=uint64_t(region.width)*4,total=rowBytes*region.height;
            const uint64_t endX=(uint64_t(region.destX)+region.width)*4;
            if(!total || total>kMaxEntry || capture.resultBytes+total>kMaxEntry || endX>region.pitch ||
               (uint64_t(region.destY)+region.height)*region.pitch>64u*1024u*1024u)return false;
            CopyCapture::Target entry{base,region.destX,region.destY,region.width,region.height,region.pitch,{}};
            // Paint writes every destination channel for these two modes. Prior
            // pixels cannot affect their result, so do not allocate/copy/hash them.
            // Alpha, multiply, overlay, screen and feather layers still depend on them.
            const bool overwrites = blit.blend == kBlendBlit ||
                (blit.blend == kBlendAsAuthored && desc.alphaBits == 0);
            if (!overwrites) {
                entry.before.resize(size_t(total));
                for(uint32_t y=0;y<entry.height;++y)
                    if(!Copy(entry.before.data()+size_t(y)*size_t(rowBytes),
                             static_cast<uint8_t*>(base)+size_t(entry.y+y)*entry.pitch+size_t(entry.x)*4,size_t(rowBytes)))return false;
            }
            const uint32_t shape[]={destinationLevel,entry.x,entry.y,entry.width,entry.height,entry.pitch};
            if(!digest.Add(shape,sizeof(shape)) ||
               (!entry.before.empty() && !digest.Add(entry.before.data(),entry.before.size())))return false;
            capture.resultBytes+=size_t(total);capture.targets.push_back(std::move(entry));return true;
        };
        Blit region=blit;uint32_t landed=0;
        for(uint32_t left=factor;left>1;left>>=1){
            if(region.srcX>=AtLeastOne(uint32_t(desc.width)>>firstLevel) ||
               region.srcY>=AtLeastOne(uint32_t(desc.height)>>firstLevel))return false;
            if(!target(region,landed))return false;
            ++landed;region.width=AtLeastOne(region.width>>1);region.height=AtLeastOne(region.height>>1);
            region.destX>>=1;region.destY>>=1;region.pitch>>=1;
        }
        for(uint32_t level=firstLevel;level<desc.levelCount;++level){
            if(region.srcX>=AtLeastOne(uint32_t(desc.width)>>level) ||
               region.srcY>=AtLeastOne(uint32_t(desc.height)>>level))return false;
            if(!target(region,landed+level-firstLevel))return false;
            region.width=AtLeastOne(region.width>>1);region.height=AtLeastOne(region.height>>1);
            region.destX>>=1;region.destY>>=1;region.srcX>>=1;region.srcY>>=1;region.pitch>>=1;
        }
        return digest.Finish(capture.key);
    }

    bool TransferCopy(const CopyCapture& capture,uint8_t* data,bool restore) noexcept
    {
        size_t offset=0;
        for(const auto& target:capture.targets){
            const size_t rowBytes=size_t(target.width)*4;
            for(uint32_t y=0;y<target.height;++y){
                auto* row=static_cast<uint8_t*>(target.base)+size_t(target.y+y)*target.pitch+size_t(target.x)*4;
                if(!wxl_copy_cache::Copy(restore?row:data+offset,restore?data+offset:row,rowBytes))return false;
                offset+=rowBytes;
            }
        }
        return true;
    }

    bool CopyChainCached(Blit original,uint32_t factor,uint32_t firstLevel,bool& painted)
    {
        CopyCapture capture;
        Blit captured=original;
        if(!CaptureCopy(captured,factor,firstLevel,capture))
            return CopyChainUncachedGuarded(original,factor,firstLevel);
        auto& cache=wxl_copy_cache::Local();
        const auto hit=cache.Find(capture.key);
        static uint32_t hits=0,misses=0;
        if(hit && hit->size()==capture.resultBytes){
            ++hits;
            if(hits<=12 || (hits%64)==0)
                WLOG_INFO("char-copy-cache: hit hits=%u misses=%u bytes=%u resident=%u",hits,misses,
                          uint32_t(capture.resultBytes),uint32_t(cache.Size()));
            // A partial write fault must request a rebuild, never blend a second
            // time over the part already restored.
            return TransferCopy(capture,const_cast<uint8_t*>(hit->data()),true);
        }
        ++misses;
        if(!CopyChainUncachedGuarded(captured,factor,firstLevel))return false;
        painted=true;
        auto data=std::make_shared<wxl_copy_cache::Bytes>(capture.resultBytes);
        if(TransferCopy(capture,data->data(),false))cache.Store(capture.key,data);
        if(misses<=8 || (misses%64)==0)
            WLOG_INFO("char-copy-cache: miss hits=%u misses=%u bytes=%u resident=%u",hits,misses,
                      uint32_t(capture.resultBytes),uint32_t(cache.Size()));
        return true;
    }

    bool CopyChainGuarded(Blit blit,uint32_t factor,uint32_t firstLevel) noexcept
    {
        if(!wxl_modern_m2::IsModernCharacterComposition())
            return CopyChainUncachedGuarded(blit,factor,firstLevel);
        bool painted=false;
        try { return CopyChainCached(blit,factor,firstLevel,painted); }
        catch(...) {
            // Cache allocation/hash failures are optional. If painting already
            // finished, do not repeat a blend merely because storing it failed.
            return painted ? true : CopyChainUncachedGuarded(blit,factor,firstLevel);
        }
    }


    // -- deciding whose copy it is ---------------------------------------------------------------

    /// Fills in what the copy needs from the source itself. Says nothing about whose copy it is:
    /// that depends on the geometry the caller was handed, which only the caller knows.
    bool Describe(uint32_t source, const uint8_t* description, Blit& blit, const char** why)
    {
        const auto Refuse = [&](const char* reason) { if (why) *why = reason; return false; };
        if (!source) return Refuse("no source");

        const auto* const entry = reinterpret_cast<const uint8_t*>(source);
        if (*reinterpret_cast<const uint32_t*>(entry + off::kOffTexEntryFlags)
            & off::kTexEntryUnreadable)
            return Refuse("the entry is marked unreadable, so its file was never opened");

        const auto* const image =
            *reinterpret_cast<const uint8_t* const*>(entry + off::kOffTexEntryImage);
        if (!image) return Refuse("the entry holds no image");

        const uint8_t encoding = image[kOffImageEncoding];
        if (encoding != kEncodingPalettised && encoding != kEncodingBlocks
            && encoding != kEncodingDirect)
            return Refuse("the image states an encoding nothing can read");

        blit.source = source;
        blit.desc = reinterpret_cast<const SourceDesc*>(description);
        blit.encoding = encoding;
        blit.palette = encoding == kEncodingPalettised ? image + kOffImagePalette : nullptr;
        blit.blend = kBlendAsAuthored;
        if (!blit.desc->levelCount) return Refuse("the description states no levels");
        return true;
    }

    /**
     * @brief Whether this copy has to be ours, given what the source is and what it is asked to fill.
     *
     * Two reasons, and only these two. A source with no palette the stock path drops outright,
     * whatever the geometry. And a magnification the stock path cannot express -- it knows one to one
     * and one to two, so anything else reads past the end of every source row -- which a layout that
     * enlarges its regions produces for every source that was already at half scale.
     *
     * Everything else stays with the stock copy, which is correct there and whose result a region we
     * paint has to sit flush against.
     */
    bool MustOwn(const Blit& blit, uint32_t factor, uint32_t stockFactor)
    {
        return blit.encoding != kEncodingPalettised || factor != stockFactor;
    }

    /**
     * @brief How many destination pixels one source pixel covers for this copy.
     *
     * The entry point already states it -- one copy means one to one, the other means one to two --
     * and for a source that holds the WHOLE arrangement that answer is the only correct one, because
     * the region is a part of such a source and its own dimensions say nothing about the scale.
     *
     * It is wrong only when the source cannot reach that far: a standalone source authored for one
     * region, at some whole fraction of it, runs out before the region is filled. That is the case
     * the enlarged arrangement creates, and it is recognised by asking whether the read the entry
     * point implies stays inside the source at all.
     */
    /// How many destination pixels one source pixel covers, or 0 when the two do not divide evenly.
    uint32_t Magnification(uint32_t destExtent, uint32_t sourceExtent)
    {
        if (!sourceExtent || destExtent < sourceExtent) return 0;
        const uint32_t factor = destExtent / sourceExtent;
        return factor * sourceExtent == destExtent ? factor : 0;
    }

    uint32_t MagnificationFor(const Blit& blit, uint32_t entryFactor, uint32_t level,
                              const uint32_t* sourceOrigin, const uint32_t* size)
    {
        const uint32_t sourceWidth = AtLeastOne(uint32_t(blit.desc->width) >> level);
        const uint32_t sourceHeight = AtLeastOne(uint32_t(blit.desc->height) >> level);

        const uint32_t reachX = sourceOrigin[0] / entryFactor + size[0] / entryFactor;
        const uint32_t reachY = sourceOrigin[1] / entryFactor + size[1] / entryFactor;
        if (reachX <= sourceWidth && reachY <= sourceHeight) return entryFactor;

        // Standalone, and short. It covers the region a whole number of times over or not at all.
        const uint32_t covering = size[0] / sourceWidth;
        return covering && covering * sourceWidth == size[0] ? covering : entryFactor;
    }

    /// One line per adopted region, wide enough to cover a whole character. Which copy ran, at which
    /// source level, and where it reads and writes is what separates "the source is not being decoded"
    /// from "it is decoded and laid somewhere else", and those two want opposite fixes.
    void Announce(const Blit& blit, uint32_t factor, uint32_t level)
    {
        static uint32_t announcementsLeft = 40;
        if (!announcementsLeft) return;
        --announcementsLeft;
        WLOG_INFO("char-tex: a %ux%u source (encoding %u, alpha %u, %u levels), from its level %u"
                  " enlarged %ux: reads at (%u,%u), writes %ux%u at (%u,%u) on a %u sheet",
                  blit.desc->width, blit.desc->height, blit.encoding, blit.desc->alphaBits,
                  blit.desc->levelCount, level, factor, blit.srcX, blit.srcY, blit.width,
                  blit.height, blit.destX, blit.destY, blit.pitch / 4);
    }

    /**
     * @brief Names a copy left to the stock path, in the same terms.
     *
     * Half of a character is composed by sources this file never adopts, and the geometry those are
     * handed is decided by a caller that reads the region arrangement itself. A picture that lands at
     * the wrong scale is the same complaint whichever path drew it, and only the two side by side say
     * which one to look at.
     */
    void AnnounceStock(const char* which, const uint8_t* description, const uint32_t* destOrigin,
                       const uint32_t* size, uint32_t level)
    {
        static uint32_t announcementsLeft = 24;
        if (!announcementsLeft || !description) return;
        --announcementsLeft;
        const auto* const desc = reinterpret_cast<const SourceDesc*>(description);
        WLOG_INFO("char-tex: the stock %s copy takes a %ux%u source (%u levels, alpha %u) from its"
                  " level %u into %ux%u at (%u,%u)",
                  which, desc->width, desc->height, desc->levelCount, desc->alphaBits, level,
                  size[0], size[1], destOrigin[0], destOrigin[1]);
    }

    uint32_t SheetPitch()
    {
        return *reinterpret_cast<const uint32_t*>(off::kCharSheetResolution) * 4;
    }

    // The origin and size arguments are the caller's own scratch copies, and the stock copies halve
    // them in place as they descend, so they are passed along mutable.
    using PasteFn = void(__cdecl*)(uint32_t source, void* const* destLevels, uint32_t* destOrigin,
                                   uint32_t* sourceOrigin, uint32_t* size,
                                   const uint8_t* description);
    PasteFn g_origPaste = nullptr;

    void __cdecl hkPaste(uint32_t source, void* const* destLevels, uint32_t* destOrigin,
                         uint32_t* sourceOrigin, uint32_t* size, const uint8_t* description)
    {
        if (!wxl_modern_m2::IsModernCharacterComposition())
        {
            g_origPaste(source, destLevels, destOrigin, sourceOrigin, size, description);
            return;
        }
        Blit blit{};
        const bool known = description && Describe(source, description, blit, nullptr);
        const uint32_t factor = known ? MagnificationFor(blit, 2, 0, sourceOrigin, size) : 0;

        // Two is what this entry point means, so anything else is a scale it cannot express.
        if (known && MustOwn(blit, factor, 2))
        {
            blit.destLevels = destLevels;
            blit.destX = destOrigin[0];
            blit.destY = destOrigin[1];
            // The source is read at a whole fraction of the destination's scale; its origin is too.
            blit.srcX = sourceOrigin[0] / factor;
            blit.srcY = sourceOrigin[1] / factor;
            blit.width = size[0];
            blit.height = size[1];
            blit.pitch = SheetPitch();
            Announce(blit, factor, 0);
            if (!CopyChainGuarded(blit, factor, 0))
                WLOG_WARN("char-tex: declined a stale magnifying source level");
            return;
        }
        AnnounceStock("magnifying", description, destOrigin, size, 0);
        g_origPaste(source, destLevels, destOrigin, sourceOrigin, size, description);
    }

    using PasteScaleFn = void(__cdecl*)(uint32_t source, void* const* destLevels,
                                        uint32_t* destOrigin, uint32_t* sourceOrigin,
                                        uint32_t* size, const uint8_t* description,
                                        uint32_t firstLevel);
    PasteScaleFn g_origPasteScale = nullptr;

    void __cdecl hkPasteScale(uint32_t source, void* const* destLevels, uint32_t* destOrigin,
                              uint32_t* sourceOrigin, uint32_t* size, const uint8_t* description,
                              uint32_t firstLevel)
    {
        if (!wxl_modern_m2::IsModernCharacterComposition())
        {
            g_origPasteScale(source, destLevels, destOrigin, sourceOrigin, size, description,
                             firstLevel);
            return;
        }
        Blit blit{};
        const bool known = description && Describe(source, description, blit, nullptr);
        const uint32_t factor = known ? MagnificationFor(blit, 1, firstLevel, sourceOrigin, size) : 0;

        // One is what this entry point means: the level it starts from is the sheet's own scale.
        if (known && MustOwn(blit, factor, 1))
        {
            blit.destLevels = destLevels;
            blit.destX = destOrigin[0];
            blit.destY = destOrigin[1];
            blit.srcX = sourceOrigin[0] / factor;
            blit.srcY = sourceOrigin[1] / factor;
            blit.width = size[0];
            blit.height = size[1];
            blit.pitch = SheetPitch();
            Announce(blit, factor, firstLevel);
            if (!CopyChainGuarded(blit, factor, firstLevel))
                WLOG_WARN("char-tex: declined a stale reducing source level");
            return;
        }
        AnnounceStock("reducing", description, destOrigin, size, firstLevel);
        g_origPasteScale(source, destLevels, destOrigin, sourceOrigin, size, description,
                         firstLevel);
    }
}

namespace wxl_modern_m2
{
    /**
     * @brief Lays part of one source over a rectangle of a composition image.
     *
     * The stock composition has no name for the part of a modern layout that holds a reworked head,
     * so nothing reaches it through the two copies above; this is how a caller that DOES know what
     * belongs there paints it, with the same decoding and the same weighting so the two halves of one
     * sheet match.
     *
     * A layer's file is NOT authored for one rectangle: it covers a whole span of the sheet, and the
     * rectangle asked for is a window onto it. So neither the magnification nor the corner to read
     * from can come from the rectangle -- both come from the span, and the two together are what puts
     * a face on a face instead of the top-left corner of a body stretched over it.
     */
    LayerPaint PaintLayer(uint32_t source, void* const* destLevels, uint32_t pitch, uint32_t blend,
                          const SheetRect& span, const SheetRect& dest)
    {
        if (!source || !destLevels || !dest.width || !dest.height) return LayerPaint::Idle;
        if (dest.x < span.x || dest.y < span.y) return LayerPaint::Idle;

        // Asking for the bytes is a separate act from naming the file, and nothing else here will ever
        // ask: the client's own sources are read because a component owns them, and these belong to no
        // component. Asked the client's own way and not by hand, because the entry is shared with the
        // thread that completes the read: this both starts the read and refuses while there is nothing
        // to describe, under the lock that makes those two one act. Reading the fields directly is the
        // same question asked with the answer changing underneath it.
        // Bounded, and only on the paths that DECLINE. A section that never lands looks the same from
        // outside whichever of these turned it away, and they want opposite fixes: a file still being
        // read fixes itself, a description that cannot be understood never will.
        static uint32_t refusalsLeft = 12;
        const auto Refuse = [&](const char* why, LayerPaint verdict) {
            if (refusalsLeft)
            {
                --refusalsLeft;
                WLOG_INFO("char-layers: %ux%u at (%u,%u) declined: %s",
                          dest.width, dest.height, dest.x, dest.y, why);
            }
            return verdict;
        };

        // Asked so that it WAITS, which is not a preference. A read is completed by a handler on the
        // same event queue, on the same thread, as the composition this runs inside -- so for as long
        // as we are composing, that handler sits on our own call stack and cannot advance anything.
        // Asking without waiting is then a pure question with nobody left to answer it: the file reads
        // as pending forever, however many rebuilds go by. Waiting pumps that handler here instead,
        // which is exactly what the client's own region painters do at this same point.
        //
        // The one thing this must never do is happen inside another such wait, which the client treats
        // as fatal. It cannot: every path here is entered from the composition itself, ahead of any
        // waiting the client does of its own.
        uint32_t description[2] = {};
        using DescribeFn = uint32_t(__cdecl*)(uint32_t source, uint32_t* outDescription, int wait);
        if (!wxl::game::Native<DescribeFn>(off::kTextureDescribe)(source, description, 1))
            return Refuse("the file is still being read", LayerPaint::Reading);

        Blit blit{};
        const char* why = "";
        if (!Describe(source, reinterpret_cast<const uint8_t*>(description), blit, &why))
            return Refuse(why, LayerPaint::Idle);
        if (!wxl::game::Native<GetLevelFn>(off::kTextureCacheGetLevel)(source, 0))
            return Refuse("the entry holds no level 0", LayerPaint::Idle);

        // The span offered is the bounding box of every section the layer names, which is right for a
        // file authored across all of them and wrong for one authored for the rectangle at hand -- and
        // the file itself says which it is. A file laid over its own span comes out at ONE factor; two
        // different factors mean it was never that shape, so the rectangle is its span instead. That
        // is also what keeps the level walk inside the sheet's own chain: at one factor the source's
        // remaining levels and the levels already spent on magnifying add up to the span's, exactly.
        const SheetRect& over =
            Magnification(span.width, blit.desc->width) == Magnification(span.height, blit.desc->height)
                ? span : dest;

        const uint32_t factor = Magnification(over.width, blit.desc->width);
        if (!factor || factor != Magnification(over.height, blit.desc->height))
        {
            if (refusalsLeft)
            {
                --refusalsLeft;
                WLOG_INFO("char-layers: a %ux%u source declined %ux%u at (%u,%u): no whole scale"
                          " reaches it, spanning %ux%u", blit.desc->width, blit.desc->height,
                          dest.width, dest.height, dest.x, dest.y, over.width, over.height);
            }
            return LayerPaint::Idle;
        }

        // Bounded, and only for what actually lands: what a layer's file IS decides whether it may
        // overwrite its section or must be laid over what is there, and the tables state neither.
        static uint32_t left = 16;
        if (left)
        {
            --left;
            WLOG_INFO("char-layers: source %ux%u encoding %u alpha %u spans %ux%u at (%u,%u),"
                      " %ux%u of it at (%u,%u) enlarged %ux",
                      blit.desc->width, blit.desc->height, blit.encoding, blit.desc->alphaBits,
                      over.width, over.height, over.x, over.y, dest.width, dest.height,
                      dest.x, dest.y, factor);
        }

        blit.blend = blend;
        blit.destLevels = destLevels;
        blit.destX = dest.x;
        blit.destY = dest.y;
        blit.srcX = (dest.x - over.x) / factor;
        blit.srcY = (dest.y - over.y) / factor;
        blit.width = dest.width;
        blit.height = dest.height;
        blit.pitch = pitch;
        if (!CopyChainGuarded(blit, factor, 0))
            return Refuse("a source level became stale during the copy", LayerPaint::Reading);
        return LayerPaint::Painted;
    }

    bool InstallCharacterTextures()
    {
        const bool magnifying = HookAttach("M2.CharPaste", off::kCharPaste, &hkPaste, &g_origPaste);
        const bool reducing = HookAttach("M2.CharPasteScale", off::kCharPasteScale, &hkPasteScale,
                                         &g_origPasteScale);

        if (!magnifying)
            WLOG_WARN("char-tex: the magnifying copy could not be reached; face and hair sources"
                      " that are not palettised will paint nothing");
        if (!reducing)
            WLOG_WARN("char-tex: the reducing copy could not be reached; body skins that are not"
                      " palettised will paint nothing");

        if (!magnifying && !reducing) return false;

        WLOG_INFO("char-tex: compressed and direct sources compose into the character sheet");
        return true;
    }
}
