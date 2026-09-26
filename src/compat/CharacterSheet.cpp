// The composited character sheet's geometry, taken from the layout the model was authored against.
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
//   A model's texture coordinates are authored against a LAYOUT, not against a texture: the tables
//   state a sheet size per layout, and the coordinates are that sheet's pixels divided by it. So two
//   sheets can be painted identically, region for region, and still put the picture in different
//   places on the model -- the size alone decides, and it is not recoverable from the regions.
//
//   The stock sheet is square, because its one resolution figure is passed as both the width and the
//   height of the allocation. A reworked model's layout is not: measured against a 512 stock sheet it
//   is four times as wide and twice as tall, which is the stock ARRANGEMENT enlarged twofold filling
//   the left half, with further regions in the right. Composed square, the reworked model reads every
//   region at a fraction of where it was painted -- which puts the band holding the face, the ear and
//   the hair under the geosets of the legs.
//
//   So the arrangement itself is unchanged and only its scale moves. The region table is multiplied
//   in place for the length of a rebuild rather than replaced from the layout's sections, because the
//   arrangement is read twice per region -- once to paint it and once to upload that part of the
//   sheet -- and scaling only the paint would compose a whole sheet and send a quarter of it.
//
//   THE SHEET TEXTURE IS NOT WHERE THE COMPOSITION WRITES. The regions paint into a CPU image the
//   client allocates ONCE at component initialisation, square and at the resolution figure, and the
//   texture is only updated from it afterwards. Enlarging the texture alone therefore changes
//   nothing, and composing at a wider pitch than that image was allocated for runs off the end of
//   it. So a sheet size of our own needs an image of our own, swapped in for the rebuild.
//
//   And the figure must never disagree with what it describes. The copies derive their pitch from it
//   rather than from anything they are handed, and both the image and the sheet outlive the rebuild
//   that made them. Hence: the size is settled once per component, everything sized from it is
//   established together before the rebuild runs, and a component whose sheet was already built
//   stock keeps composing stock.

#include "../ExtensionApi.hpp"
#include "GilneanPreview.hpp"
#include "SheetBlockTraversal.hpp"

#include "game/Binding.hpp"
#include "offsets/game/M2.hpp"
#include "wxl/AppearanceApi.h"

#include <cstdint>
#include <cstring>
#include <unordered_map>

namespace
{
    namespace off = wxl::offsets::game::m2;

    const WXL_AppearanceApi* g_appearance = nullptr;
    bool g_characterGearVisible = true;
    uint32_t g_modernVariationRegions = 0;
    bool g_bodyLayersReading = false;

    const WXL_AppearanceApi* Appearance()
    {
        if (!g_appearance)
            g_appearance = static_cast<const WXL_AppearanceApi*>(
                wxl_modern_m2::g_api->GetInterface("wxl.appearance", WXL_APPEARANCE_API_VERSION));
        return g_appearance;
    }

    uint32_t& SheetResolution()
    {
        return *reinterpret_cast<uint32_t*>(off::kCharSheetResolution);
    }

    /**
     * @brief The sheet the tables describe for one character, or nothing.
     *
     * Asked per character rather than once, because the layout belongs to the ChrModel: a client
     * showing a reworked race beside a stock one wants two sheets of different shapes, and a size
     * resolved once would be whichever of them was built first.
     */
    bool WantedSheet(const void* component, uint32_t& outWidth, uint32_t& outHeight,
                     uint32_t& outLayout)
    {
        const WXL_AppearanceApi* tables = Appearance();
        if (!tables || !component) return false;

        const auto* const bytes = static_cast<const uint8_t*>(component);
        const uint32_t race = *reinterpret_cast<const uint32_t*>(bytes + off::kOffCharComponentRace);
        uint32_t sex  = *reinterpret_cast<const uint32_t*>(bytes + off::kOffCharComponentSex);
        uint32_t retailRace = wxl_modern_m2::RetailCharacterRace(race);
        const bool alternate = wxl_modern_m2::AlternateFormIdentity(component, retailRace, sex);
        if (!wxl_modern_m2::RetailCharacterCanaryAllows(alternate ? (retailRace == 23 ? 12 : retailRace == 70 ? 17 : 30) : race, sex)) return false;
        const uint32_t chrModel = tables->ChrModelForRace(retailRace, sex);
        if (!chrModel) return false;

        outLayout = tables->LayoutForModel(chrModel);
        if (!outLayout) return false;

        if (!tables->LayoutSize(outLayout, &outWidth, &outHeight)) return false;
        return outWidth != 0 && outHeight != 0;
    }

    // --- the image the composition writes into -----------------------------------------------------

    /// (pixelFormat, width, height).
    using AllocMippedImgFn = void*(__cdecl*)(uint32_t format, uint32_t width, uint32_t height);

    void*& ComposeImage()           { return *reinterpret_cast<void**>(off::kCharComposeImage); }
    void*& ComposeImageCompressed() { return *reinterpret_cast<void**>(off::kCharComposeImageCompressed); }
    void*& ComposeImageThreaded()   { return *reinterpret_cast<void**>(off::kCharComposeImageThreaded); }

    /// One set of composition images per sheet size ever asked for. Kept for the process, exactly as
    /// the client keeps its own: they are scratch, one character at a time uses them, and a client
    /// only ever sees a handful of distinct layouts.
    struct ComposeImages
    {
        uint32_t width, height;
        void* plain;
        void* compressed;
        void* threaded;
    };
    ComposeImages g_images[4] = {};

    /**
     * @brief The composition images for one sheet size, allocating them the first time.
     *
     * The client allocates its own ONCE, at component initialisation, square and at the resolution
     * figure -- and that image, not the sheet texture, is what every region paints into. A layout
     * that asks for a different sheet therefore needs its own, or the paint writes rows the image
     * does not have.
     *
     * The compressed and threaded images are mirrored only where the client has them: allocating a
     * pair it never had would be harmless but pointless, and leaving one out where it does have it
     * would send that path to an image of the wrong size.
     */
    const ComposeImages* ImagesFor(uint32_t width, uint32_t height)
    {
        for (const ComposeImages& set : g_images)
            if (set.width == width && set.height == height) return set.plain ? &set : nullptr;

        for (ComposeImages& set : g_images)
        {
            if (set.plain) continue;

            const auto alloc = wxl::game::Native<AllocMippedImgFn>(off::kTextureAllocMippedImg);
            set.width = width;
            set.height = height;
            set.plain = alloc(off::kCharComposeFormat, width, height);
            if (ComposeImageCompressed()) set.compressed = alloc(0, width, height);
            if (ComposeImageThreaded())   set.threaded = alloc(off::kCharComposeFormat, width, height);

            if (!set.plain)
                WLOG_WARN("char-sheet: no %ux%u composition image could be allocated; this character"
                          " composes at the stock size", width, height);
            return set.plain ? &set : nullptr;
        }
        return nullptr;
    }

    /**
     * @brief The client's own composition images, noted once so they can be put back for a stock
     *        character rather than at the end of every rebuild.
     *
     * They are allocated once at initialisation and never replaced, so noting them the first time we
     * are about to swap is enough for the life of the process.
     */
    ComposeImages g_stock = {};

    void RememberStockImages()
    {
        if (g_stock.plain) return;
        g_stock.plain = ComposeImage();
        g_stock.compressed = ComposeImageCompressed();
        g_stock.threaded = ComposeImageThreaded();
    }

    /// Points the client's composition globals at one set of images, ours or its own. Only the ones it
    /// actually has: giving it a compressed or threaded image where it had none would send that path
    /// somewhere it never went.
    void InstallComposeImages(const ComposeImages* set)
    {
        const ComposeImages& use = set ? *set : g_stock;
        if (!use.plain) return;

        ComposeImage() = use.plain;
        if (g_stock.compressed && use.compressed) ComposeImageCompressed() = use.compressed;
        if (g_stock.threaded && use.threaded)     ComposeImageThreaded() = use.threaded;
    }

    // --- what each component's sheet actually IS ---------------------------------------------------

    // Track every live sized component, including world NPCs and thumbnail models.
    // A fixed 32-entry table silently lost HD dimensions in populated cities while
    // hkCreateBaseTexture still allocated the larger texture. Deferred composition
    // then selected stock scratch and uploaded it using the HD texture's pitch.
    // unordered_map preserves pointers to existing entries across rehash; a nested
    // allocation must not invalidate ComposeSheetForOwner's SizedSheet pointer.
    struct SizedSheet
    {
        const void* component;
        uint32_t width;
        uint32_t height;
        uint32_t layout;
        uint32_t race;
        uint32_t sex;
    };
    std::unordered_map<const void*, SizedSheet> g_sized;

    SizedSheet* FindSized(const void* component)
    {
        const auto found = g_sized.find(component);
        return found == g_sized.end() ? nullptr : &found->second;
    }

    void RememberSized(const void* component, uint32_t width, uint32_t height, uint32_t layout,
                       uint32_t race, uint32_t sex)
    {
        if (!component) return;
        g_sized[component] = { component, width, height, layout, race, sex };
    }

    // --- the allocation ---------------------------------------------------------------------------

    /// The sheet being composed right now, or zeroes while it is a stock one.
    uint32_t g_sheetWidth = 0;
    uint32_t g_sheetHeight = 0;
    bool g_nativeRegionsPainted = false;
    void* g_dragonPaintDestination = nullptr;
    uint32_t g_dragonPaintRegion = 0xFFFFFFFFu;
    bool g_dragonPaintComplete = false;

    /// The component whose sheet is being composed right now, or null. The region paint needs it to
    /// tell the character's own body -- painted from the modern tables instead below -- from a worn
    /// item's, which carries no component of its own to look one up by.
    void* g_composing = nullptr;

    using CompressSheetFn = void(__cdecl*)(void* const*, void* const*);
    CompressSheetFn g_origCompressSheet = nullptr;

    // Native BC1 block encoder: source in ECX, pitch and destination on the stack,
    // caller pops both arguments. This is the ABI used by 4E9C30, not __fastcall.
    __declspec(noinline) void EncodeBlock(const uint8_t* source, uint32_t pitch, uint8_t* destination)
    {
        const uintptr_t entry = 0x004E7030;
        __asm {
            push destination
            push pitch
            mov ecx, source
            call entry
            add esp, 8
        }
    }

    void __cdecl hkCompressSheet(void* const* source, void* const* destination)
    {
        if (!g_composing || !g_sheetWidth || !g_sheetHeight || g_sheetWidth == g_sheetHeight ||
            source != ComposeImage() || destination != ComposeImageCompressed())
        {
            g_origCompressSheet(source, destination);
            return;
        }
        const uint32_t compressionStarted = GetTickCount();
        // 4E9BA0 walks width/4 rows as well as columns. AllocMippedImg really allocates
        // width x height, so the stock loop reads and writes past a landscape sheet.
        // Walk the same full mip chain as native allocator 6AB700, with separate axes.
        uint32_t width = g_sheetWidth, height = g_sheetHeight, level = 0;
        do
        {
            if (!wxl_modern_m2::sheet::CompressLevel(static_cast<const uint8_t*>(source[level]),
                    width, height, static_cast<uint8_t*>(destination[level]), EncodeBlock))
            {
                WLOG_ERROR("char-sheet: rejected invalid compression mip=%u size=%ux%u", level, width, height);
                return; // Never fall back to the unsafe square walker for this image.
            }
            ++level;
            if (width == 1 && height == 1) break;
            width = width > 1 ? width / 2 : 1;
            height = height > 1 ? height / 2 : 1;
        } while (true);
        const uint32_t compressionElapsed = GetTickCount() - compressionStarted;
        if (compressionElapsed >= 20)
            WLOG_INFO("body-compression-timing: owner=%p size=%ux%u elapsed_ms=%u",
                      g_composing, g_sheetWidth, g_sheetHeight, compressionElapsed);
        static unsigned reports = 0;
        if (reports++ < 8)
            WLOG_INFO("char-sheet: compressed rectangular %ux%u sheet in %u bounded mip levels",
                g_sheetWidth, g_sheetHeight, level);
    }

    /// The height the sheet about to be created wants, or zero. Carried across the one call because
    /// the allocation is handed the resolution figure for BOTH of its axes and cannot say otherwise.
    uint32_t g_pendingHeight = 0;

    /// (width, height, format, usage, flags, owner, filler, name, one).
    using TextureCreateSizedFn = void*(__cdecl*)(uint32_t width, uint32_t height, uint32_t format,
                                                 uint32_t usage, uint32_t flags, void* owner,
                                                 void* filler, const char* name, uint32_t one);
    TextureCreateSizedFn g_origTextureCreate = nullptr;

    /// The one name the character sheet is created under. Every other creation reaching here belongs
    /// to something else and is passed through untouched.
    constexpr const char* kSheetName = "CharacterBaseSkin";

    /// The sheet of the character being composed, once it exists. Held so that the one thing sharing
    /// this window with it -- an unrelated texture handed to the same call -- is never mistaken for it.
    void* g_currentSheet = nullptr;

    /// The device texture returned for g_currentSheet. The stock compositor obtains this correctly
    /// clamped view and then separately passes its square resolution to GxTexUpdate, so the upload
    /// boundary must recognise the device object as well as its higher-level sheet handle.
    void* g_currentGxSheet = nullptr;

    void* __cdecl hkTextureCreateSized(uint32_t width, uint32_t height, uint32_t format,
                                       uint32_t usage, uint32_t flags, void* owner, void* filler,
                                       const char* name, uint32_t one)
    {
        const bool ours = g_pendingHeight && name && std::strcmp(name, kSheetName) == 0;
        if (ours) height = g_pendingHeight;

        void* const created =
            g_origTextureCreate(width, height, format, usage, flags, owner, filler, name, one);
        if (ours) g_currentSheet = created;
        return created;
    }

    /// (handle, one, zero, left, top, right, bottom, one).
    using GetGxTexFn = void*(__cdecl*)(void* handle, uint32_t one, uint32_t zero, uint32_t left,
                                       uint32_t top, uint32_t right, uint32_t bottom, uint32_t also);
    GetGxTexFn g_origGetGxTex = nullptr;

    /// Central texture upload(texture, left, top, right, bottom, flag). Passing only the texture is
    /// not a harmless shorthand: the native cdecl entry reads the other five arguments from whatever
    /// follows it on the caller's stack. On a 2048x1024 character sheet that produced a spurious
    /// 2048x2048 upload, faulted in the deferred callback, and left the replaceable body texture with
    /// no GPU resource. The scene builder then omitted every body batch while separately attached
    /// equipment continued to draw.
    using TexUpdateFn = void(__cdecl*)(void* gxTexture, int left, int top,
                                       int right, int bottom, int flag);
    TexUpdateFn g_origSheetTextureUpdate = nullptr;

    /**
     * @brief Keeps the stock compositor's square upload inside this Retail sheet's real rectangle.
     *
     * TextureGetGxTex and TextureUpdate receive the bounds independently. Clamping the former is not
     * enough: stock passes its single resolution figure to the latter again, producing 2048x2048 for
     * a layout whose device texture is 2048x1024. Restricted by exact device-object identity and by
     * the active composition scope so unrelated textures and later recycled pointers are untouched.
     */
    void __cdecl hkSheetTextureUpdate(void* gxTexture, int left, int top,
                                      int right, int bottom, int flag)
    {
        if (g_sheetWidth && g_sheetHeight && gxTexture && gxTexture == g_currentGxSheet)
        {
            const int wantedRight = static_cast<int>(g_sheetWidth);
            const int wantedBottom = static_cast<int>(g_sheetHeight);
            const int oldRight = right;
            const int oldBottom = bottom;
            if (right > wantedRight) right = wantedRight;
            if (bottom > wantedBottom) bottom = wantedBottom;

            static uint32_t reports = 0;
            if (reports < 8 && (right != oldRight || bottom != oldBottom))
            {
                ++reports;
                WLOG_INFO("char-sheet: clamped native sheet upload %d,%d..%d,%d to %d,%d..%d,%d",
                          left, top, oldRight, oldBottom, left, top, right, bottom);
            }
        }
        g_origSheetTextureUpdate(gxTexture, left, top, right, bottom, flag);
    }

    struct CompletedResend
    {
        const void* owner = nullptr;
        uint32_t width = 0, height = 0, thread = 0;
        void* plain = nullptr;
        void* compressed = nullptr;
    };
    CompletedResend g_completedResend;

    struct CompletedResendScope
    {
        CompletedResend previous = g_completedResend;
        CompletedResendScope(const void* owner, uint32_t width, uint32_t height, bool complete)
        {
            g_completedResend = {};
            if (!complete || !owner || g_composing != owner ||
                width != g_sheetWidth || height != g_sheetHeight) return;
            const auto* bytes = static_cast<const uint8_t*>(owner);
            const uint32_t format = *reinterpret_cast<const uint32_t*>(bytes + 0x14);
            // Keep native source selection. With a worker-cache entry it would
            // read that cache rather than the atlas we have just completed.
            if (*reinterpret_cast<void* const*>(bytes + 0x52c) || !ComposeImage() ||
                (format != 2 && format != 6) || (format == 6 && !ComposeImageCompressed())) return;
            g_completedResend = {owner, width, height, GetCurrentThreadId(),
                                 ComposeImage(), ComposeImageCompressed()};
        }
        ~CompletedResendScope() { g_completedResend = previous; }
    };

    bool CompletedResendOwns(uint32_t action, uint32_t width, uint32_t height, const void* owner)
    {
        return action == 0 && owner && owner == g_completedResend.owner && owner == g_composing &&
            width == g_completedResend.width && height == g_completedResend.height &&
            width == g_sheetWidth && height == g_sheetHeight &&
            GetCurrentThreadId() == g_completedResend.thread &&
            ComposeImage() == g_completedResend.plain &&
            ComposeImageCompressed() == g_completedResend.compressed;
    }

    /// Sends the whole sheet to the card again. The client already sent it once, from inside the
    /// rebuild and before anything of ours touched the half it has no name for; a second send is what
    /// makes that half reach the model at all.
    void ResendSheet(const void* component, uint32_t width, uint32_t height, bool complete)
    {
        void* const sheet = *reinterpret_cast<void* const*>(
            static_cast<const uint8_t*>(component) + off::kOffCharComponentSheet);
        if (!sheet) return;

        // Native action 0 normally recomposes the sheet. This explicit resend
        // already has the completed atlas; retain native mip sourcing/finalizers
        // and suppress only that redundant rebuild during this synchronous call.
        const CompletedResendScope completed{component, width, height, complete};
        void* const rect = wxl::game::Native<GetGxTexFn>(off::kTextureGetGxTex)(
            sheet, 1, 0, 0, 0, width, height, 1);
        if (rect)
            wxl::game::Native<TexUpdateFn>(off::kGxTexUpdate)(
                rect, 0, 0, static_cast<int>(width), static_cast<int>(height), 1);
    }

    /**
     * @brief Names the part of a texture about to be sent to the card, within the sheet's real bounds.
     *
     * The section walk asks for the whole sheet as a SQUARE of the resolution figure, because that
     * figure is the only size the stock composition has. On a layout that is taller than it is square
     * -- or shorter -- the rectangle names rows the sheet does not own.
     *
     * Narrowed to this character's own sheet rather than clamping whatever passes through: this call
     * serves every texture in the client, and a rectangle that legitimately exceeds OUR sheet's size
     * belongs to something else entirely. Widening it to every sheet we ever sized cost the client its
     * character screen outright, so it stays exactly this narrow until something explains why.
     *
     * Recognised by its SHAPE as well, because identity is not always available: the sheet is created
     * partway through the rebuild that first needs it, and a send issued before we have seen it made
     * matches nothing. The square whole-sheet send has one form and only one -- the whole of both axes
     * taken from the single resolution figure -- and that figure is ours only while we are composing.
     * A texture that genuinely wants a square of the overridden width, during that window, does not
     * exist in this client.
     */
    void* __cdecl hkGetGxTex(void* handle, uint32_t one, uint32_t zero, uint32_t left, uint32_t top,
                             uint32_t right, uint32_t bottom, uint32_t also)
    {
        const bool composing = g_sheetWidth != 0 && g_sheetHeight != 0;
        const bool wholeSheet = composing && left == 0 && top == 0
                             && right == SheetResolution() && bottom == SheetResolution();

        const bool currentSheet = handle && handle == g_currentSheet;
        if (wholeSheet || currentSheet)
        {
            if (right > g_sheetWidth) right = g_sheetWidth;
            if (bottom > g_sheetHeight) bottom = g_sheetHeight;
        }
        void* const gxTexture =
            g_origGetGxTex(handle, one, zero, left, top, right, bottom, also);
        if ((wholeSheet || currentSheet) && gxTexture) g_currentGxSheet = gxTexture;
        return gxTexture;
    }

    using CreateBaseTextureFn = void(__fastcall*)(void* component, void* edx);
    CreateBaseTextureFn g_origCreateBaseTexture = nullptr;

    /// The only moment the sheet's size can be chosen. The figure is already this character's by the
    /// time this runs -- the rebuild below established it -- so only the height has to be said, since
    /// the allocation is handed the figure for both of its axes.
    void __fastcall hkCreateBaseTexture(void* component, void* edx)
    {
        // World form changes can allocate here before RenderPrep establishes a
        // composition scope. Resolve the dimensions at allocation, not only paint:
        // otherwise a 512x512 device texture later receives our 2048x1024 atlas.
        const uint32_t previousResolution = SheetResolution();
        const uint32_t previousPendingHeight = g_pendingHeight;
        uint32_t width = 0, height = 0, layout = 0;
        const bool sized = WantedSheet(component, width, height, layout);
        if (sized)
        {
            SheetResolution() = width;
            g_pendingHeight = height;
            const auto* bytes = static_cast<const uint8_t*>(component);
            RememberSized(component, width, height, layout,
                *reinterpret_cast<const uint32_t*>(bytes + off::kOffCharComponentRace),
                *reinterpret_cast<const uint32_t*>(bytes + off::kOffCharComponentSex));
        }
        else
            g_pendingHeight = g_sheetHeight;
        g_origCreateBaseTexture(component, edx);
        g_pendingHeight = previousPendingHeight;
        SheetResolution() = previousResolution;
        static uint32_t reports = 0;
        if (sized && reports++ < 24)
            WLOG_INFO("char-sheet: allocated owner=%p layout=%u size=%ux%u inComposition=%u",
                component, layout, width, height, g_composing == component);
    }

    // Verified against 12340's 4EFDF0: eight cdecl arguments. The mip
    // is argument 5, owner 6, pitch 7 and pixel pointer 8 (no extra Y argument).
    using SheetSourceFn = void(__cdecl*)(uint32_t, uint32_t, uint32_t, uint32_t,
                                        uint32_t, void*, uint32_t*, void**);
    SheetSourceFn g_origSheetSource = nullptr;

    void __cdecl hkSheetSource(uint32_t action, uint32_t width, uint32_t height,
                               uint32_t extra, uint32_t mip, void* component,
                               uint32_t* pitch, void** pixels)
    {
        static uint32_t reports = 0;
        const bool report = FindSized(component) && reports < 512 &&
                            (action == 0 || (action == 1 && mip == 0));
        if (report)
        {
            ++reports;
            WLOG_INFO("body-source-trace: begin owner=%p action=%u mip=%u size=%ux%u scope=%p scopeSize=%ux%u",
                      component, action, mip, width, height, g_composing, g_sheetWidth, g_sheetHeight);
        }
        const uint32_t sourceStarted = GetTickCount();
        if (!CompletedResendOwns(action, width, height, component))
            g_origSheetSource(action, width, height, extra, mip, component, pitch, pixels);
        else if (report)
            WLOG_INFO("char-sheet-resend: reused completed atlas owner=%p size=%ux%u", component, width, height);
        if (report)
        {
            uint32_t rowPitch = 0, sample = 0, sampledHash = 2166136261u, nonzero = 0, format = 0;
            void* data = nullptr;
            void* cachedImage = nullptr;
            bool sampled = false;
            __try {
                if (action == 1 && pitch && pixels) {
                    rowPitch = *pitch; data = *pixels;
                    format = *reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(component) + 0x14);
                    cachedImage = *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(component) + 0x52c);
                    if (data && rowPitch >= 4) {
                        sample = *static_cast<const uint32_t*>(data);
                        const uint32_t rows = format == 6 ? (height + 3) / 4 : height;
                        if (rows && rowPitch <= 32768 && rows <= 8192) {
                            for (uint32_t y = 0; y < 8; ++y)
                                for (uint32_t x = 0; x < 8; ++x) {
                                    const auto* row = static_cast<const uint8_t*>(data) + ((rows - 1) * y / 7) * rowPitch;
                                    const uint32_t word = *reinterpret_cast<const uint32_t*>(row + (((rowPitch / 4) - 1) * x / 7) * 4);
                                    sampledHash = (sampledHash ^ word) * 16777619u;
                                    nonzero += word != 0;
                                }
                            sampled = true;
                        }
                    }
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
            WLOG_INFO("body-source-trace: end owner=%p action=%u pitch=%u pixels=%p first=%08x",
                      component, action, rowPitch, data, sample);
            WLOG_INFO("body-source-detail: owner=%p action=%u format=%u cache=%p plain=%p compressed=%p sampled=%u nonzero=%u hash=%08x elapsed_ms=%u",
                      component, action, format, cachedImage, ComposeImage(), ComposeImageCompressed(),
                      sampled, nonzero, sampledHash, GetTickCount() - sourceStarted);
        }
    }

    using FreeComponentFn = void(__cdecl*)(void* component);
    FreeComponentFn g_origFreeComponent = nullptr;

    /// Forgotten with the component, so a later one allocated at the same address is never taken for
    /// it. Sizing a stock character's sheet from a reworked character's layout is exactly the
    /// disagreement this file exists to prevent.
    void __cdecl hkFreeComponent(void* component)
    {
        wxl_modern_m2::ForgetCharacterEquipment(component);
        g_sized.erase(component);
        g_origFreeComponent(component);
    }

    // --- one rebuild, at the size its sheet really is ----------------------------------------------

    constexpr uint32_t kRectWords = off::kCharRegionCount * 4;

    uint32_t* RegionRects()
    {
        return reinterpret_cast<uint32_t*>(off::kCharRegionRects);
    }

    /**
     * @brief Enlarges the region arrangement to the layout's scale, and gives back what to restore.
     *
     * Done to the TABLE and not to a copy handed to the copies, because the arrangement is read twice
     * per region: once to paint it and once to upload that part of the sheet to the card. Scaling
     * only the paint would compose a whole sheet and send a quarter of it.
     */
    void ScaleRegions(uint32_t scale, uint32_t* saved)
    {
        uint32_t* const rects = RegionRects();
        for (uint32_t i = 0; i < kRectWords; ++i)
        {
            saved[i] = rects[i];
            rects[i] *= scale;
        }
    }

    void RestoreRegions(const uint32_t* saved)
    {
        uint32_t* const rects = RegionRects();
        for (uint32_t i = 0; i < kRectWords; ++i) rects[i] = saved[i];
    }

    // The native background worker paints into a stock square cache while reading
    // the same region table and sheet-resolution globals used by RenderPrep. It
    // cannot run alongside our rectangular composition scopes. Select the native
    // synchronous mode at initialization, before a worker or cache job exists.
    // Do not flip B6B4E8 on a running worker: that is also its shutdown flag.
    using InitSheetFn = bool(__cdecl*)(uint32_t format, uint32_t resolutionBits,
                                      uint32_t threaded, uint32_t compression);
    InitSheetFn g_origInitSheet = nullptr;

    bool __cdecl hkInitSheet(uint32_t format, uint32_t resolutionBits,
                            uint32_t threaded, uint32_t compression)
    {
        const bool result = g_origInitSheet(format, resolutionBits, 0, compression);
        WLOG_INFO("char-sheet-sync: init requested_worker=%u actual_worker=%u resolution_bits=%u result=%u",
                  threaded, *reinterpret_cast<const uint32_t*>(0x00B6B4E8), resolutionBits, result);
        return result;
    }

    using SyncSheetFn = void(__fastcall*)(void* component, void* edx);
    SyncSheetFn g_origSyncSheet = nullptr;
    void* g_sheetScopeOwner = nullptr;

    struct SheetOwnerScope
    {
        void* previous = g_sheetScopeOwner;
        explicit SheetOwnerScope(void* component) { g_sheetScopeOwner = component; }
        ~SheetOwnerScope() { g_sheetScopeOwner = previous; }
    };

    using RenderPrepFn = uint32_t(__fastcall*)(void* component, void* edx, uint32_t flags);
    RenderPrepFn g_origRenderPrep = nullptr;

    /**
     * @brief One character's whole rebuild, and the scope its sheet size is valid over.
     *
     * The figure is moved for the WHOLE rebuild and not around each region, because the copies derive
     * their destination pitch from it themselves and not every one of them is reached through the
     * region paint: an equipment overlay calls them directly, and a pitch that was right for the skin
     * and stock for the overlay would run every piece of armour off the end of the sheet.
     *
     * The size adopted is the one this component's sheet WAS created at, except in the one rebuild
     * that creates it -- recognised by the component still holding no sheet -- where the layout's own
     * size is adopted and then remembered. A rebuild whose sheet predates this file therefore keeps
     * composing at the size that sheet really has, which is what the figure and the allocation
     * disagreeing costs: rows of one width written into a buffer of another.
     */
    /// Whether this call is the one that actually recomposes. RenderPrep is asked on every frame and
    /// answers most of them by doing nothing; resolving the tables regardless costs a full walk of
    /// them per character per frame, which is felt immediately.
    bool Recomposing(const void* component, uint32_t flags)
    {
        return flags != 0 && component
            && (*(static_cast<const uint8_t*>(component) + off::kOffCharComponentRebuild)
                & off::kCharRebuildSheet) != 0;
    }

    // The native synchronous composer at 0x4EE0D0 selects regions with
    // component+0x0C, independently of the sheet rebuild bit at +0x08.
    // Our full-atlas upload uses shared scratch images: a partial region update
    // cannot preserve the untouched pixels belonging to this character.
    void PrepareOwnedSheetRegions(void* component, bool modern, bool rebuild)
    {
        if (!component || !modern || !rebuild) return;
        auto& regions = *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(component) + 0x0C);
        regions |= (1u << off::kCharRegionCount) - 1u;
    }

    struct SheetBuildTiming
    {
        void* component;
        uint32_t started = GetTickCount();
        ~SheetBuildTiming()
        {
            const uint32_t elapsed = GetTickCount() - started;
            if (elapsed >= 20) WLOG_INFO("race-switch-sheet-timing: component=%p elapsed_ms=%u", component, elapsed);
        }
    };

    uint32_t ComposeSheetForOwner(void* component, void* edx, uint32_t flags,
                                  RenderPrepFn compose)
    {
        const SheetOwnerScope ownerScope{component};
        const SheetBuildTiming timing{component};
        const uint32_t stock = SheetResolution();

        SizedSheet* sized = FindSized(component);
        const auto* const componentBytes = static_cast<const uint8_t*>(component);
        const uint32_t componentRace = component
            ? *reinterpret_cast<const uint32_t*>(componentBytes + off::kOffCharComponentRace) : 0;
        const uint32_t componentSex = component
            ? *reinterpret_cast<const uint32_t*>(componentBytes + off::kOffCharComponentSex) : 0;
        const bool sheetPending = component && !*reinterpret_cast<const uint32_t*>(
            componentBytes + off::kOffCharComponentSheet);

        // Glue recycles one component while the user walks the race list. Component identity alone
        // is therefore not a sheet-layout key: retaining the previous race's layout paints valid
        // body sources into valid but wrong rectangles. Refresh on every race/sex generation change.
        // Racial transformations retain race/sex but replace the model and may
        // recreate its sheet. Resolve the layout again for that allocation/rebuild.
        if (sized && (sized->race != componentRace || sized->sex != componentSex ||
                      sheetPending || Recomposing(component, flags)))
        {
            uint32_t wantedWidth = 0, wantedHeight = 0, wantedLayout = 0;
            if (!WantedSheet(component, wantedWidth, wantedHeight, wantedLayout))
                wantedLayout = 0;
            if (wantedLayout && sized->race == componentRace && sized->sex == componentSex &&
                sized->layout == wantedLayout && sized->width == wantedWidth && sized->height == wantedHeight) {
                // Same recipe layout: leave the existing allocation metadata intact.
            }
            else if (wantedLayout && (sheetPending
                    || (wantedWidth == sized->width && wantedHeight == sized->height)))
            {
                const uint32_t oldRace = sized->race;
                const uint32_t oldSex = sized->sex;
                *sized = { component, wantedWidth, wantedHeight, wantedLayout,
                           componentRace, componentSex };
                *reinterpret_cast<uint8_t*>(static_cast<uint8_t*>(component)
                    + off::kOffCharComponentRebuild) |= off::kCharRebuildSheet;
                WLOG_INFO("char-sheet: recycled component=%p refreshed race=%u/%u -> %u/%u"
                          " layout=%u size=%ux%u", component, oldRace, oldSex,
                          componentRace, componentSex, wantedLayout, wantedWidth, wantedHeight);
            }
            else
            {
                WLOG_WARN("char-sheet: recycled component=%p cannot change allocated %ux%u sheet"
                          " from race=%u/%u to race=%u/%u", component, sized->width,
                          sized->height, sized->race, sized->sex, componentRace, componentSex);
            }
        }

        uint32_t width = sized ? sized->width : 0;
        uint32_t height = sized ? sized->height : 0;
        uint32_t layout = sized ? sized->layout : 0;

        if (!sized && sheetPending && WantedSheet(component, width, height, layout))
        {
            RememberSized(component, width, height, layout, componentRace, componentSex);
            sized = FindSized(component);
        }

        const uint32_t scale = (height && stock) ? height / stock : 0;
        const ComposeImages* const images =
            (width && scale >= 2) ? ImagesFor(width, height) : nullptr;

        static bool announced = false;
        if (images && !announced)
        {
            announced = true;
            WLOG_INFO("char-sheet: composing %ux%u, as the model's own layout states, instead of the"
                      " stock %ux%u square", width, height, stock, stock);
        }

        uint32_t savedRects[kRectWords] = {};

        RememberStockImages();
        // Installed for whoever is composing NOW and left installed, stock characters included. The
        // sheet's source callback reads this global at the moment the card asks for pixels, which is
        // after this call has returned: putting it back here handed the card the stock image for a
        // sheet composed into ours, and the composition only ever appeared once something else had
        // reinstalled it -- which is what changing character did.
        InstallComposeImages(images);

        if (images)
        {
            g_sheetWidth = width;
            g_sheetHeight = height;
            g_composing = component;
            g_modernVariationRegions = 0;
            g_bodyLayersReading = false;
            g_nativeRegionsPainted = false;
            g_dragonPaintDestination = nullptr;
            g_dragonPaintRegion = 0xFFFFFFFFu;
            g_dragonPaintComplete = false;
            SheetResolution() = width;
            ScaleRegions(scale, savedRects);

            // Null while the sheet is still to be created; the creation itself records it.
            g_currentSheet = *reinterpret_cast<void* const*>(
                static_cast<const uint8_t*>(component) + off::kOffCharComponentSheet);
            g_currentGxSheet = nullptr;
        }

        wxl_modern_m2::CustomizeNoteComponent(component);

        const bool recomposing = Recomposing(component, flags);
        PrepareOwnedSheetRegions(component, images != nullptr, recomposing);
        const uint32_t result = compose(component, edx, flags);

        // RenderPrep can perform a delayed native paint after consuming the initial
        // rebuild bit. For dragon layout 155 that paint restores Human bootstrap
        // pixels over the atlas. Observe the actual region calls as well as flags.
        const bool restoresDelayedAtlas = layout == 155 || layout == 113 || layout == 114 ||
            layout == 145 || layout == 146 || layout == 123 || layout == 124 || layout == 129 || layout == 130 ||
            layout == 125 || layout == 126 || layout == 127 || layout == 128 ||
            layout == 157 || layout == 158; // Dracthyr visage: preserve scales after delayed native face paint.
        // OwnDragonBodyRegion already paints before native compression. Repeating
        // that full atlas pass here duplicates painting, compression and upload.
        const bool dragonAlreadyPainted = images && layout == 155 && g_dragonPaintComplete &&
            g_dragonPaintDestination == images->plain;
        if (images && layout && !dragonAlreadyPainted && (recomposing || (restoresDelayedAtlas && g_nativeRegionsPainted)))
        {
            g_currentSheet = *reinterpret_cast<void* const*>(
                static_cast<const uint8_t*>(component) + off::kOffCharComponentSheet);
            const auto* const bytes = static_cast<const uint8_t*>(component);
            const uint32_t race = *reinterpret_cast<const uint32_t*>(bytes + off::kOffCharComponentRace);
            const uint32_t sex  = *reinterpret_cast<const uint32_t*>(bytes + off::kOffCharComponentSex);
            // How far the enlarged arrangement reaches, read off the table we scaled ourselves
            // rather than assumed: everything beyond it is the layout's own and nobody else's.
            uint32_t stockRight = 0;
            for (uint32_t r = 0; r < off::kCharRegionCount; ++r)
            {
                const uint32_t* const rect = RegionRects() + r * 4;
                if (rect[0] + rect[2] > stockRight) stockRight = rect[0] + rect[2];
            }
            // Dragon layout 155 uses section 11 for the complete 2048x1024 body.
            // It overlaps every legacy region, so it cannot be substituted region-by-region.
            // This owned actor wears only shoulder/waist attachments, not body-sheet armor.
            uint32_t alternateRace = 0, alternateSex = sex;
            const bool dragonAtlas = layout == 155 &&
                wxl_modern_m2::AlternateFormIdentity(component, alternateRace, alternateSex) &&
                (alternateRace == 52 || alternateRace == 70);
            if (dragonAtlas) stockRight = 0; // Replay the FULL authored body recipe, once, after legacy paint.
            uint32_t reading = 0;
            if (wxl_modern_m2::PaintModernLayers(component, race, sex, layout,
                                                 static_cast<void* const*>(images->plain),
                                                 width, height, stockRight,
                                                 wxl_modern_m2::kEverySection, reading))
            {
                // Retail's final layer pass happens after the stock compressor. Refresh the
                // compressed image as well, or the upload still sees the pre-Retail pixels.
                if (width != height && *reinterpret_cast<const uint32_t*>(bytes + 0x14) == 6 && images->compressed)
                    hkCompressSheet(static_cast<void* const*>(images->plain),
                                    static_cast<void* const*>(images->compressed));
                // A finished head pass is not proof that this scope painted the body.
                // Let the native callback regenerate missing regions rather than
                // label another actor's shared scratch pixels a completed atlas.
                const uint32_t bodyMask = (1u << off::kCharStockRegionCount) - 1u;
                const bool complete = reading == 0 && !g_bodyLayersReading &&
                    (g_modernVariationRegions & bodyMask) == bodyMask;
                ResendSheet(component, width, height, complete);
            }

            // A composition is asked for the moment a character appears, which is before any of the
            // files its layers name have been read. The first one therefore paints little or nothing,
            // and nothing else would ever ask again -- so the sheet keeps whatever it managed, and the
            // face only fills in when something unrelated forces another rebuild. Asking for one here
            // ends when the reads do: a file that cannot be read stops being reported as reading.
            if (reading)
                *(static_cast<uint8_t*>(component) + off::kOffCharComponentRebuild) |=
                    off::kCharRebuildSheet;
        }

        // A body source can still be loading even when the separate head pass is ready.
        // Preserve that retry after the native compositor has consumed its dirty bit.
        if (images && g_bodyLayersReading)
            *reinterpret_cast<uint8_t*>(static_cast<uint8_t*>(component) + off::kOffCharComponentRebuild) |= off::kCharRebuildSheet;
        g_bodyLayersReading = false;
        if (images)
        {
            RestoreRegions(savedRects);
            g_currentSheet = nullptr;
            g_currentGxSheet = nullptr;
            g_modernVariationRegions = 0;
        }
        SheetResolution() = stock;
        g_sheetWidth = 0;
        g_sheetHeight = 0;
        g_composing = nullptr;
        g_nativeRegionsPainted = false;
        return result;
    }

    uint32_t __fastcall hkRenderPrep(void* component, void* edx, uint32_t flags)
    {
        if (g_sheetScopeOwner)
        {
            // A nested resource pump must not replace another character's atlas
            // globals. Leave it pending for its next render-preparation call.
            if (g_sheetScopeOwner != component) return 0;
            return g_origRenderPrep(component, edx, flags);
        }
        return ComposeSheetForOwner(component, edx, flags, g_origRenderPrep);
    }

    // Native 12340's synchronous RenderPrep calls 4ED6D0(force=1) before
    // SyncSheet. SyncSheet consumes the textures and releases their handles.
    // A queued partial update prepares only its original dirty regions before
    // entering our hook. PrepareOwnedSheetRegions subsequently widens that job
    // to a full atlas, whose other regions still have IDs/masks but null handles.
    // Reacquire through the native owner after widening, before painting; never
    // retain released pointers or replay item/attachment application.
    using PrepareSheetLayersFn = bool(__fastcall*)(void*, void*, uint32_t);
    uint32_t __fastcall ComposeDeferredSheet(void* component, void* edx, uint32_t)
    {
        if (g_composing == component && g_sheetWidth && g_sheetHeight &&
            Recomposing(component, 1))
        {
            wxl::game::Native<PrepareSheetLayersFn>(0x004ED6D0)(component, nullptr, 1);
            static uint32_t reports = 0;
            if (reports++ < 32)
                WLOG_INFO("char-sheet: prepared clothing for expanded deferred atlas owner=%p", component);
        }
        g_origSyncSheet(component, edx);
        return 1;
    }

    void __fastcall hkSyncSheet(void* component, void* edx)
    {
        if (g_sheetScopeOwner)
        {
            // The immediate RenderPrep path already owns the correct dimensions.
            // A different owner stays queued instead of borrowing this scope.
            if (g_sheetScopeOwner == component) g_origSyncSheet(component, edx);
            return;
        }
        // Native queue processing calls 4F14A0 directly, bypassing RenderPrep.
        // Give this delayed path the same layout, images, Retail painting and
        // restoration as an immediate rebuild without repeating native prep.
        ComposeSheetForOwner(component, edx, 1, &ComposeDeferredSheet);
    }

    // --- the one decision the square assumption reaches into ---------------------------------------

    using DescribeFn = void(__cdecl*)(uint32_t source, uint16_t* outDescription, uint32_t one);
    using HasMipsFn = int(__cdecl*)(uint32_t source);
    using PasteFn = void(__cdecl*)(uint32_t source, void* destLevels, const uint32_t* destOrigin,
                                   const uint32_t* sourceOrigin, const uint32_t* size,
                                   const uint16_t* description);
    using PasteScaleFn = void(__cdecl*)(uint32_t source, void* destLevels,
                                        const uint32_t* destOrigin, const uint32_t* sourceOrigin,
                                        const uint32_t* size, const uint16_t* description,
                                        uint32_t firstLevel);

    using PaintRegionFn = void(__cdecl*)(uint32_t region, uint32_t source, void* destLevels);
    PaintRegionFn g_origPaintRegion = nullptr;
    PaintRegionFn g_origPaintOverlayRegion = nullptr;

    /// True for one of the stock race/customization variation textures kept on the component itself.
    /// The first handle is the base skin; the rest are face/torso/pelvis pieces from the same legacy
    /// CharSections recipe. Retail's DB2 recipe replaces that complete set, while equipped item
    /// textures arrive from separate storage and must continue to paint over it.
    bool IsComponentVariationTexture(const void* component, uint32_t source)
    {
        if (!component || !source) return false;
        const auto* const textures = reinterpret_cast<const uint32_t*>(
            static_cast<const uint8_t*>(component) + off::kOffCharComponentTextures);
        constexpr uint32_t kVariationTextureCount =
            off::kCharRegionCount * off::kSectionSlotCount;
        for (uint32_t i = 0; i < kVariationTextureCount; ++i)
            if (textures[i] == source) return true;
        return false;
    }

    /// The client walks its ten regions arm through foot in the same order a layout numbers its
    /// section types, and only the tail disagrees: the client keeps one scalp region while a layout
    /// splits it into an upper and a lower section, which is why every region past the split is one
    /// section type further along than its index.
    uint32_t SectionTypeForRegion(uint32_t region)
    {
        return region < 8 ? region : region + 1;
    }

    /**
     * @brief Copies one region of one source into the sheet, at the layout's scale and on the right
     *        axis.
     *
     * The stock choice compares the source against the resolution figure twice, once per axis, which
     * is only the sheet's own shape while the sheet is square. The height is the axis that still
     * means what the figure meant: a source authored at the sheet's scale matches it there whatever
     * the layout does horizontally, and asking on the height alone leaves every square case deciding
     * exactly as before.
     *
     * A source at half the enlarged scale -- which is what the stock-sized skin files are -- lands on
     * the doubling copy, and that copy reads its source at HALF the destination origin. Since the
     * arrangement was enlarged by exactly two, halving gives the stock rectangle back, which is where
     * the picture sits in those files. The enlargement and the doubling meet without a third step.
     */
    // Dragon's full-body atlas has no legacy arm/leg/face regions. The native
    // filler may run again during upload; replacing its inputs, rather than only
    // repainting after RenderPrep, prevents Human bootstrap pixels from returning.
    bool OwnDragonBodyRegion(uint32_t region, void* destLevels)
    {
        if (!g_composing || !destLevels) return false;
        uint32_t retailRace=0, sex=0;
        if (!wxl_modern_m2::AlternateFormIdentity(g_composing,retailRace,sex) ||
            (retailRace!=52 && retailRace!=70)) return false;
        const SizedSheet* sized=FindSized(g_composing);
        if (!sized || sized->layout!=155) return false;
        // All native body-region writes are suppressed below. Paint each destination
        // once per RenderPrep, even if the native region walk restarts.
        if (destLevels!=g_dragonPaintDestination)
        {
            uint32_t reading=0;
            const uint32_t painted=wxl_modern_m2::PaintModernLayers(g_composing,
                retailRace==70 ? 17 : 30, sex, sized->layout,
                static_cast<void* const*>(destLevels),sized->width,sized->height,0,
                wxl_modern_m2::kEverySection,reading);
            if (reading || !painted)
                *reinterpret_cast<uint8_t*>(static_cast<uint8_t*>(g_composing)+off::kOffCharComponentRebuild) |= off::kCharRebuildSheet;
            g_dragonPaintComplete = painted != 0 && reading == 0;
            g_dragonPaintDestination=destLevels;
        }
        g_dragonPaintRegion=region;
        return true; // Never fall back to Human skin, including while assets load.
    }

    bool OwnAppendageFaceRegion(uint32_t region, void* destLevels)
    {
        if (!g_sheetHeight || !g_composing || !destLevels || region < 8 || region >= 10)
            return false;
        const SizedSheet* sized = FindSized(g_composing);
        if (!sized || (sized->layout != 113 && sized->layout != 114 &&
                       sized->layout != 145 && sized->layout != 146 &&
                       sized->layout != 123 && sized->layout != 124 &&
                       sized->layout != 129 && sized->layout != 130 &&
                       sized->layout != 157 && sized->layout != 158)) return false;
        const auto* bytes = static_cast<const uint8_t*>(g_composing);
        const uint32_t race = *reinterpret_cast<const uint32_t*>(bytes + off::kOffCharComponentRace);
        const uint32_t sex = *reinterpret_cast<const uint32_t*>(bytes + off::kOffCharComponentSex);
        uint32_t reading = 0;
        // Native face cells overlap the authored neck/tail area. Replace those
        // writes at their actual destination, before compression, as for dragon
        // composition. Only the reserved gap and right-hand Retail head are
        // painted; all eight clothing/body rectangles remain intact.
        const uint32_t painted = wxl_modern_m2::PaintModernLayers(g_composing, race, sex,
            sized->layout, static_cast<void* const*>(destLevels), sized->width, sized->height,
            1024, wxl_modern_m2::kEverySection, reading);
        if (reading || !painted)
            *reinterpret_cast<uint8_t*>(static_cast<uint8_t*>(g_composing) + off::kOffCharComponentRebuild) |= off::kCharRebuildSheet;
        g_nativeRegionsPainted = true;
        return true;
    }

    void __cdecl hkPaintRegion(uint32_t region, uint32_t source, void* destLevels);

    // 12340 has two cdecl(region, source, destination) painters: 4F07D0
    // samples a full body sheet; 4F08A0 samples standalone face/clothing pieces.
    // Both feed the same atlas. The latter bypasses hkPaintRegion entirely.
    void __cdecl hkPaintOverlayRegion(uint32_t region, uint32_t source, void* destLevels)
    {
        if (OwnAppendageFaceRegion(region, destLevels)) return;
        // Standalone face/equipment passes can run after the initial rebuild bit
        // was consumed. Observe those writes too so reserved atlas cells are
        // restored before this component is uploaded again.
        if (g_sheetHeight && g_composing) g_nativeRegionsPainted = true;
        if (g_sheetHeight && region < off::kCharRegionCount &&
            OwnDragonBodyRegion(region, destLevels))
        {
            g_nativeRegionsPainted = true;
            return;
        }
        // These families retain legacy naked torso/pelvis overlays whose skin
        // choice differs from the selected Retail recipe. Standalone overlays
        // bypass the full-sheet painter, so route body-variation handles through
        // the same per-region replacement. Equipment retains its own origin.
        if (g_sheetHeight && region < off::kCharStockRegionCount && g_composing &&
            IsComponentVariationTexture(g_composing, source))
        {
            const uint32_t race = *reinterpret_cast<const uint32_t*>(
                static_cast<const uint8_t*>(g_composing) + off::kOffCharComponentRace);
            const uint32_t sex = *reinterpret_cast<const uint32_t*>(
                static_cast<const uint8_t*>(g_composing) + off::kOffCharComponentSex);
            if (race == 12 || race == 29 || race == 31 || race == 22 || race == 26 ||
                race == 17 || race == 30 || (race == 1 && sex == 1))
            {
                hkPaintRegion(region, source, destLevels);
                return;
            }
        }
        g_origPaintOverlayRegion(region, source, destLevels);
    }

    void __cdecl hkPaintRegion(uint32_t region, uint32_t source, void* destLevels)
    {
        if (OwnAppendageFaceRegion(region, destLevels)) return;
        if (!g_sheetHeight || region >= off::kCharRegionCount)
        {
            g_origPaintRegion(region, source, destLevels);
            return;
        }

        if (g_composing) g_nativeRegionsPainted = true;
        if (OwnDragonBodyRegion(region, destLevels)) return;
        const bool componentVariation = IsComponentVariationTexture(g_composing, source);
        uint32_t composingRace = 0;
        uint32_t composingSex = 0;
        if (g_composing)
        {
            const auto* const bytes = static_cast<const uint8_t*>(g_composing);
            composingRace = *reinterpret_cast<const uint32_t*>(
                bytes + off::kOffCharComponentRace);
            composingSex = *reinterpret_cast<const uint32_t*>(
                bytes + off::kOffCharComponentSex);
        }

        // Standalone torso/pelvis variations are handled by hkPaintOverlayRegion above.
        // Kul Tiran is carried through the stock client on the Naga model alias. Its torso and upper-
        // leg base handles are not consistently retained in the component's legacy variation array,
        // so the membership test above misses exactly those regions and lets Naga/legacy pixels cover
        // the correct Retail atlas. The client always paints the body before worn items. For this one
        // aliased race, therefore, treat the first call of each stock body region as its base-body pass;
        // later calls remain classified normally so equipment still lands over the replacement.
        const uint32_t regionBit = region < 32 ? (1u << region) : 0;
        const bool kulTiranFirstBodyPass = composingRace == 31 && regionBit &&
            !(g_modernVariationRegions & regionBit);
        // Worgen and Gilnean share a private race but replace the model and legacy
        // skin handles. Seed each body region from the current form's recipe even
        // when the first native source is absent from the recycled variation array.
        // Only the first full-sheet body pass is substituted; equipment follows it.
        const SizedSheet* bodySized = FindSized(g_composing);
        const bool worgenFirstBodyPass = composingRace == 12 && bodySized &&
            bodySized->layout >= 125 && bodySized->layout <= 128 && regionBit &&
            !(g_modernVariationRegions & regionBit);

        // The first variation source is the character's base body. Other component variation
        // handles are its legacy face/torso/pelvis layers; equipped items are not in this array.
        // Hide Gear therefore preserves every body variation while suppressing actual equipment.
        if (!g_characterGearVisible && g_composing &&
            !componentVariation)
            return;

        // The client paints two things into every region: the character's own body first, then
        // whatever is worn over it. The worn items are described correctly however reworked the model
        // is -- their layers name section types, not this client's regions -- but the body is still
        // the client's own legacy source, laid out for the stock arrangement it was authored against,
        // which a reworked model's coordinates read at the wrong place entirely.
        //
        // So the one source this file knows to distrust is swapped for the modern tables' own
        // description of the same body, at the exact point the client would have painted it. Nothing
        // about the surrounding order moves: this region's worn-item calls still come right after,
        // into the same destination, and land over whichever of the two got there first.
        //
        // Falling through rather than returning is the answer to every case that is not a clean
        // substitution -- a section the tables do not describe, or one whose file has not been read
        // yet. The copy below then lays the client's own source at this sheet's scale, which is a
        // picture in the wrong layout where ours is nothing at all, and the next rebuild replaces it.
        //
        // Only the regions the stock arrangement reaches. The two beyond it are the scalp, and the
        // pass at the end of the rebuild already paints those from the same tables: substituting here
        // as well would lay every one of their layers twice, which for a blend that is not a straight
        // copy means applying it to its own result.
        if (region < off::kCharStockRegionCount && g_composing &&
            (componentVariation || kulTiranFirstBodyPass || worgenFirstBodyPass))
        {
            const SizedSheet* const sized = FindSized(g_composing);
            if (sized && sized->layout)
            {
                // A region does not necessarily begin with textures[0]: torso and pelvis commonly
                // begin with their own variation handle. Paint the complete Retail recipe on the
                // first variation call for EACH region, then suppress the remaining legacy
                // face/torso/pelvis sources for that region so they cannot cover it again.
                uint32_t alternateRace = 0, alternateSex = composingSex;
                const bool alternate = wxl_modern_m2::AlternateFormIdentity(g_composing, alternateRace, alternateSex);
                if ((g_modernVariationRegions & regionBit) &&
                    wxl_modern_m2::RetailCharacterCanaryAllows(alternate ? 12 : composingRace, composingSex))
                    return;

                uint32_t reading = 0;
                uint32_t stockRight = 0;
                for (uint32_t r = 0; r < off::kCharStockRegionCount; ++r)
                {
                    const uint32_t* const rect = RegionRects() + r * 4;
                    if (rect[0] + rect[2] > stockRight) stockRight = rect[0] + rect[2];
                }
                const uint32_t painted =
                    wxl_modern_m2::PaintModernLayers(g_composing, composingRace, composingSex, sized->layout,
                                                     static_cast<void* const*>(destLevels),
                                                     sized->width, sized->height, stockRight,
                                                     SectionTypeForRegion(region), reading);
                if (composingRace == 12)
                {
                    g_bodyLayersReading |= reading != 0;
                    static uint32_t bodyDiagnostics = 0;
                    if (bodyDiagnostics++ < 128)
                        WLOG_INFO("worgen-body: owner=%p layout=%u region=%u variation=%u first=%u painted=%u reading=%u",
                            g_composing, sized->layout, region, componentVariation, worgenFirstBodyPass, painted, reading);
                }
                if (painted || reading)
                {
                    g_modernVariationRegions |= regionBit;
                    return;
                }
            }
        }

        if (!source || !wxl::game::Native<HasMipsFn>(off::kTextureCacheHasMips)(source)) return;

        // Eight bytes, not six: the stock painter clears a byte past the description the filler
        // writes, which is what leaves every source composed as opaque however much alpha it states.
        uint16_t description[4] = { 0, 0, 0, 0 };
        wxl::game::Native<DescribeFn>(off::kTextureDescribe)(source, description, 1);
        reinterpret_cast<uint8_t*>(description)[5] = 0;

        // Already at the layout's scale: the rebuild enlarged the table before anything read it.
        const auto* const rect = reinterpret_cast<const uint32_t*>(
            off::kCharRegionRects + region * off::kCharRegionRectStride);

        const uint32_t sourceWidth = description[0];
        const uint32_t sourceHeight = description[1];

        if (sourceHeight < g_sheetHeight && sourceWidth < g_sheetWidth)
        {
            wxl::game::Native<PasteFn>(off::kCharPaste)(source, destLevels, rect, rect, rect + 2,
                                                        description);
            return;
        }

        uint32_t firstLevel = 0;
        for (uint32_t h = sourceHeight; h > g_sheetHeight; h >>= 1) ++firstLevel;
        wxl::game::Native<PasteScaleFn>(off::kCharPasteScale)(source, destLevels, rect, rect,
                                                              rect + 2, description, firstLevel);
    }
}

namespace wxl_modern_m2
{
    bool IsModernCharacterComposition()
    {
        return g_composing != nullptr && g_sheetWidth != 0 && g_sheetHeight != 0;
    }

    void SetCharacterSheetGearVisible(bool visible)
    {
        g_characterGearVisible = visible;
    }

    bool InstallCharacterSheet()
    {
        // This extension loads before character-compositor initialization. Refuse
        // to introduce the global layout overrides if that ordering ever changes.
        const unsigned char initBytes[] = {0x55, 0x8b, 0xec, 0x53, 0x56, 0x57, 0x33, 0xff};
        const unsigned char syncBytes[] = {0x56, 0x8b, 0xf1, 0xf6, 0x46, 0x08, 0x04};
        const unsigned char prepareBytes[] = {0x55, 0x8b, 0xec, 0x83, 0xec, 0x74};
        if (ComposeImage() ||
            std::memcmp(reinterpret_cast<const void*>(0x004ED6D0), prepareBytes, sizeof(prepareBytes)) ||
            std::memcmp(reinterpret_cast<const void*>(0x004F1A20), initBytes, sizeof(initBytes)) ||
            std::memcmp(reinterpret_cast<const void*>(0x004F14A0), syncBytes, sizeof(syncBytes)) ||
            !HookAttach("M2.CharInitSynchronous", 0x004F1A20, &hkInitSheet, &g_origInitSheet) ||
            !HookAttach("M2.CharDeferredSheetScope", 0x004F14A0, &hkSyncSheet, &g_origSyncSheet))
        {
            WLOG_ERROR("char-sheet-sync: early native synchronous path unavailable; sheet overrides disabled");
            return false;
        }
        const unsigned char compressor[] = {0x55, 0x8b, 0xec, 0x83, 0xec, 0x1c};
        if (std::memcmp(reinterpret_cast<const void*>(0x004E9BA0), compressor, sizeof(compressor)) ||
            !HookAttach("M2.CharCompressRectangularSheet", 0x004E9BA0, &hkCompressSheet, &g_origCompressSheet))
        {
            WLOG_ERROR("char-sheet: rectangular compression hook unavailable; sheet overrides disabled");
            return false;
        }
        const unsigned char sourceBytes[] = {0x55, 0x8b, 0xec, 0x8b, 0x45, 0x08};
        if (std::memcmp(reinterpret_cast<const void*>(0x004EFDF0), sourceBytes, sizeof(sourceBytes)) ||
            !HookAttach("M2.CharBodySourceTrace", 0x004EFDF0, &hkSheetSource, &g_origSheetSource))
        {
            WLOG_ERROR("body-source-trace: verified source callback unavailable");
            return false;
        }
        if (!HookAttach("Gx.TextureCreateSized", off::kTextureCreateSized, &hkTextureCreateSized,
                        &g_origTextureCreate))
        {
            WLOG_WARN("char-sheet: sized texture creation could not be reached; the sheet stays"
                      " square and a reworked model reads every region at twice its position");
            return false;
        }

        if (!HookAttach("M2.CharCreateBaseTexture", off::kCharCreateBaseTexture,
                        &hkCreateBaseTexture, &g_origCreateBaseTexture))
        {
            WLOG_WARN("char-sheet: the sheet's allocation could not be reached; its size stays the"
                      " stock one");
            return false;
        }

        if (!HookAttach("Gx.TextureGetGxTex", off::kTextureGetGxTex, &hkGetGxTex, &g_origGetGxTex))
        {
            WLOG_WARN("char-sheet: the texture upload rectangle could not be reached; a sheet that is"
                      " not square would be sent as one and read rows it does not own");
            return false;
        }

        if (!HookAttachByName("Gx.TextureUpdate", &hkSheetTextureUpdate,
                              &g_origSheetTextureUpdate))
        {
            WLOG_WARN("char-sheet: the native upload rectangle could not be reached; a non-square"
                      " Retail sheet may still be uploaded as a square");
            return false;
        }

        if (!HookAttach("M2.CharRenderPrep", off::kCharRenderPrep, &hkRenderPrep, &g_origRenderPrep))
        {
            WLOG_WARN("char-sheet: the rebuild could not be reached; nothing would tell the copies"
                      " which sheet they are painting into, so the size stays the stock one");
            return false;
        }

        if (!HookAttach("M2.CharFreeComponent", off::kCharFreeComponent, &hkFreeComponent,
                        &g_origFreeComponent))
            WLOG_WARN("char-sheet: component release could not be reached; a sheet size may outlive"
                      " the character it belonged to");

        if (!HookAttach("M2.CharPaintRegion", off::kCharPaintRegion, &hkPaintRegion,
                        &g_origPaintRegion))
            WLOG_WARN("char-sheet: the region paint could not be reached; on a non-square sheet the"
                      " stock scale choice doubles sources that are already at scale");

        if (!HookAttach("M2.CharPaintOverlayRegion", 0x004F08A0,
                        &hkPaintOverlayRegion, &g_origPaintOverlayRegion))
            return false;

        WLOG_INFO("char-sheet: the composited sheet takes its size from the model's own layout");
        return true;
    }
}
