// Index-buffer assembly for skins whose sections carry a 32-bit index start.
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
//   The index buffer is filled block by block: the draw list is walked, and every section that is
//   showing has its own run of the skin's index array appended after the last one. Each block is
//   located by the section's index start, read as a plain 16-bit field.
//
//   A character or item-component skin holds well past 65535 indices and states that start across two
//   fields, the high half riding in `level`. Reading only the low half sources every block past the
//   16-bit line from the wrong place. Measured on a shipped race model: each such block held the data
//   belonging to `start mod 65536` instead of `start`, so its triangles joined vertices from an
//   unrelated part of the body while the blocks below the line came out exactly right. That is what
//   makes the result read as a model with some correct pieces rather than as nothing at all.
//
//   The draw side reads each section's start straight out of the section (M2Draw.cpp rebuilds the
//   full 32-bit value there). So the two halves agree only when a block actually sits at the offset
//   its section names -- which the append walk does not guarantee, because packing only the showing
//   blocks moves every later one whenever the geosets change. Copying the array whole, at its own
//   offsets and regardless of visibility, is what makes that agreement hold; nothing is drawn that was
//   not asked for, because visibility decides which blocks are DRAWN, not which ones exist.

#include "../ExtensionApi.hpp"
#include "../compat/BoneBudget.hpp"
#include "../compat/ModernM2.hpp"

#include "engine/assets/shared/models/m2/M2Format.hpp"
#include "game/Binding.hpp"
#include "game/M2.hpp"
#include "offsets/engine/Gx.hpp"
#include "offsets/game/M2.hpp"

#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

namespace
{
    namespace m2    = wxl::offsets::game::m2;
    namespace gxoff = wxl::offsets::engine::gx;
    namespace bn    = wxl::modern::assets::common::bones;

    m2::M2_SetModelIndicesFn g_origSetModelIndices = nullptr;
    using SharedSetIndicesFn = uint32_t(__fastcall*)(void* shared, void* edx);
    using SharedDestroyBuffersFn = void(__fastcall*)(void* shared, void* edx);
    SharedSetIndicesFn g_origSharedSetIndices = nullptr;
    SharedDestroyBuffersFn g_origSharedDestroyBuffers = nullptr;

    // kSharedSetIndices owns the GxBuf at this field; locking it returns the uint16 index stream.
    constexpr size_t kOffSharedIndexBuf = 0x17C;

    /// Models this detour has already spoken about once. The assembly runs again whenever the buffer
    /// goes dirty, and one line per model is the whole point.
    std::unordered_set<const void*> g_builtReported;

    // A built/valid shared buffer is rebound on every model switch.  Remember which concrete buffer
    // has already had its extended starts repaired so those rebinds do not copy hundreds of thousands
    // of indices every frame.  SharedDestroyBuffers removes the entry before either pointer is freed.
    std::unordered_map<const void*, const void*> g_correctedSharedBuffers;
    std::unordered_set<const void*> g_sharedFailureReported;

    template <class T>
    T Field(const void* base, size_t at)
    {
        return *reinterpret_cast<const T*>(static_cast<const uint8_t*>(base) + at);
    }

    /// Bounded so the entry witness cannot become the log.
    uint32_t g_reportsLeft = 8;

    /**
     * @brief Rebuilds the stream kSharedSetIndices wrote, reading every section's full source start.
     *
     * The stock builder at 0x0083619F reads only section.indexStart.  Retail character/component
     * skins keep its high half in section.level, so sections above index 65535 are copied from the
     * wrong source block even though the draw side later reconstructs a 32-bit start.  The original
     * is called first to retain its allocation, dirty-flag and binding behavior; this function only
     * replaces the locked uint16 payload and the runtime section copy's resulting output starts.
     */
    bool CorrectSharedIndices(void* shared, const char* stem)
    {
        auto* const skin = wxl::game::m2::M2Model(shared).GetSkin();
        auto* const header = wxl::game::m2::M2Model(shared).GetHeader();
        auto* const sections = skin ? skin->submeshes : nullptr;
        auto* const copy = *reinterpret_cast<wxl::structure::m2::M2SkinSection**>(
            static_cast<uint8_t*>(shared) + m2::kOffModelSubmeshBuf);
        void* const buffer = Field<void*>(shared, kOffSharedIndexBuf);
        void* const device = *reinterpret_cast<void**>(gxoff::kGxDevicePtr);
        const uint32_t coInstances = Field<uint32_t>(shared, m2::kOffSharedCoInstanceCount);
        if (!skin || !header || !sections || !copy || !buffer || !device || !skin->indices ||
            !skin->indexCount || !skin->submeshCount || skin->submeshCount > bn::kMaxBatches ||
            !coInstances)
            return false;

        const void* const cache = Field<void*>(shared, 0x04);
        const uint32_t cacheFlags = cache ? Field<uint32_t>(cache, 0x04) : 0;
        const bool rigidSingleBone = header->bones.count == 1 &&
                                     (cacheFlags & 0x40u) != 0;
        const bool globalIndices = (cacheFlags & 0x08u) != 0 || rigidSingleBone;
        // This correction deliberately owns only the exact form an absolute whole-array upload can
        // represent.  The native finalizer seeds character/component shared models at one co-instance;
        // the rebased or multi-instance forms need their own section-major rewrite and remain native.
        if (!globalIndices || coInstances != 1)
            return false;

        for (uint32_t si = 0; si < skin->submeshCount; ++si)
        {
            const auto& section = sections[si];
            const uint32_t sourceStart = bn::FullIndexStart(section, true);
            if (sourceStart > skin->indexCount ||
                section.indexCount > skin->indexCount - sourceStart)
                return false;
        }

        void* const* vtbl = *reinterpret_cast<void* const* const*>(device);
        auto* const dst = static_cast<uint16_t*>(
            reinterpret_cast<m2::Gx_BufLockFn>(vtbl[m2::kGxVtblBufLock / sizeof(void*)])(
                device, buffer));
        if (!dst) return false;

        std::memcpy(dst, skin->indices,
                    static_cast<size_t>(skin->indexCount) * sizeof(uint16_t));
        reinterpret_cast<m2::Gx_BufUnlockFn>(vtbl[m2::kGxVtblBufUnlock / sizeof(void*)])(
            device, buffer, 0);

        // kSharedSetIndices draws through the finalized copy at +0x18C.  Undo its packed low-half
        // rewrite and publish the source's complete absolute start, now that the buffer is a complete
        // copy at those exact offsets.
        for (uint32_t si = 0; si < skin->submeshCount; ++si)
        {
            copy[si].level = sections[si].level;
            copy[si].indexStart = sections[si].indexStart;
        }

        *reinterpret_cast<uint8_t*>(static_cast<uint8_t*>(buffer) + m2::kOffGxBufBuilt) = 1;
        wxl::game::Native<m2::Gx_PrimIndexPtrFn>(m2::kPrimIndexPtr)(device, buffer);
        g_correctedSharedBuffers[shared] = buffer;
        WLOG_INFO("m2-indices: corrected shared '%s' sections=%u indices=%u"
                  " coInstances=%u form=whole",
                  stem ? stem : "(no stem)", skin->submeshCount, skin->indexCount,
                  coInstances);
        return true;
    }

    uint32_t __fastcall hkSharedSetIndices(void* shared, void* edx)
    {
        const char* const stem = shared
            ? wxl::game::m2::M2Model(shared).GetPathStem() : nullptr;
        const bool registered = shared && wxl::modern::assets::m2::IsNativeLoaded(shared);
        const bool extended = stem && bn::UsesExtendedIndexStart(stem);
        if (!registered || !extended)
            return g_origSharedSetIndices(shared, edx);

        void* const before = Field<void*>(shared, kOffSharedIndexBuf);
        const auto found = g_correctedSharedBuffers.find(shared);
        const bool alreadyCorrect = before && found != g_correctedSharedBuffers.end() &&
                                    found->second == before &&
                                    Field<uint8_t>(before, m2::kOffGxBufBuilt) &&
                                    Field<uint8_t>(before, m2::kOffGxBufValid);

        const uint32_t result = g_origSharedSetIndices(shared, edx);
        if (!result || alreadyCorrect) return result;

        if (!CorrectSharedIndices(shared, stem) && g_sharedFailureReported.insert(shared).second)
            WLOG_WARN("m2-indices: could not correct shared extended index stream for '%s'",
                      stem ? stem : "(no stem)");
        return result;
    }

    void __fastcall hkSharedDestroyBuffers(void* shared, void* edx)
    {
        g_correctedSharedBuffers.erase(shared);
        g_sharedFailureReported.erase(shared);
        g_builtReported.erase(shared);
        g_origSharedDestroyBuffers(shared, edx);
    }

    uint32_t __fastcall hkSetModelIndices(void* instance, void* edx)
    {
        void* const shared = Field<void*>(instance, m2::kOffInstShared);
        const char* stem = shared ? wxl::game::m2::M2Model(shared).GetPathStem() : nullptr;
        const bool registered = shared && wxl::modern::assets::m2::IsNativeLoaded(shared);
        const bool extended = stem && bn::UsesExtendedIndexStart(stem);

        // Reported at ENTRY and for the FIRST few calls of any kind, not for the interesting models
        // only: scoping the witness to models this detour cares about makes silence mean two opposite
        // things at once, "no character ever came through" and "nothing comes through at all", and the
        // second is the one that says the detour is attached to something that never runs.
        if (g_reportsLeft)
        {
            --g_reportsLeft;
            void* const probeCtx = Field<void*>(instance, m2::kOffInstGeometryCtx);
            void* const probeBuf = probeCtx ? Field<void*>(probeCtx, m2::kOffGeoCtxIndexBuf) : nullptr;
            WLOG_INFO("m2-indices: entry '%s' shared=%d registered=%d extended=%d ctx=%d buf=%d"
                      " built=%d valid=%d",
                      stem ? stem : "(no stem)", shared != nullptr, registered, extended,
                      probeCtx != nullptr, probeBuf != nullptr,
                      probeBuf ? Field<uint8_t>(probeBuf, m2::kOffGxBufBuilt) : 0,
                      probeBuf ? Field<uint8_t>(probeBuf, m2::kOffGxBufValid) : 0);
        }

        if (!registered || !extended) return g_origSetModelIndices(instance, edx);

        void* const ctx  = Field<void*>(instance, m2::kOffInstGeometryCtx);
        auto* const skin = wxl::game::m2::M2Model(shared).GetSkin();
        auto* const header = wxl::game::m2::M2Model(shared).GetHeader();
        if (!ctx || !skin || !header || !skin->indices || !skin->indexCount)
            return g_origSetModelIndices(instance, edx);

        // Which of the two output forms the stock assembly would use is a property of the client's
        // configuration, not of this model. The other one renumbers each block against a running
        // vertex restarted per draw group, which no whole-array copy can express: a model reaching
        // here on that form keeps the stock assembly, and says so rather than being quietly wrong.
        const void* const cache = Field<void*>(shared, 0x04);
        const uint32_t cacheFlags = cache ? Field<uint32_t>(cache, 0x04) : 0;
        const bool rigidSingleBone = header->bones.count == 1 && cache &&
                                     (Field<uint8_t>(cache, 0x04) & 0x40) != 0;
        if ((cacheFlags & 0x08) == 0 && !rigidSingleBone)
        {
            if (g_builtReported.insert(shared).second)
                WLOG_WARN("m2-indices: '%s' is assembled through the per-group rebase form; its blocks"
                          " keep the truncated start", stem);
            return g_origSetModelIndices(instance, edx);
        }

        void* const buffer = Field<void*>(ctx, m2::kOffGeoCtxIndexBuf);
        void* const device = *reinterpret_cast<void**>(gxoff::kGxDevicePtr);
        if (!buffer || !device) return g_origSetModelIndices(instance, edx);

        // Both flags set means the contents still describe what is being drawn; the stock assembly
        // only reaches for the buffer when one of them is clear, and rebinding is all that is left.
        if (Field<uint8_t>(buffer, m2::kOffGxBufBuilt) && Field<uint8_t>(buffer, m2::kOffGxBufValid))
        {
            wxl::game::Native<m2::Gx_PrimIndexPtrFn>(m2::kPrimIndexPtr)(device, buffer);
            return 1;
        }

        void* const* vtbl = *reinterpret_cast<void* const* const*>(device);
        auto* const dst = static_cast<uint16_t*>(
            reinterpret_cast<m2::Gx_BufLockFn>(vtbl[m2::kGxVtblBufLock / sizeof(void*)])(device, buffer));
        if (!dst) return 0;

        std::memcpy(dst, skin->indices, static_cast<size_t>(skin->indexCount) * sizeof(uint16_t));

        if (g_builtReported.insert(shared).second)
            WLOG_INFO("m2-indices: built '%s' %u index(es), every block at the start its section names",
                      stem, skin->indexCount);

        reinterpret_cast<m2::Gx_BufUnlockFn>(vtbl[m2::kGxVtblBufUnlock / sizeof(void*)])(device, buffer, 0);
        *reinterpret_cast<uint8_t*>(static_cast<uint8_t*>(buffer) + m2::kOffGxBufBuilt) = 1;
        wxl::game::Native<m2::Gx_PrimIndexPtrFn>(m2::kPrimIndexPtr)(device, buffer);
        return 1;
    }
}

namespace wxl_modern_m2
{
    bool InstallModelIndices()
    {
        bool installed = true;
        if (!HookAttachByName("M2.SetModelIndices", &hkSetModelIndices, &g_origSetModelIndices))
        {
            WLOG_WARN("m2-indices: the index assembly could not be reached; skins past 65535 indices"
                      " keep sourcing their blocks from a truncated start");
            installed = false;
        }
        if (!HookAttachByName("M2.SharedSetIndices", &hkSharedSetIndices, &g_origSharedSetIndices))
        {
            WLOG_WARN("m2-indices: the shared index assembly could not be reached; shared character"
                      " sections past 65535 indices keep sourcing the truncated block");
            installed = false;
        }
        if (!HookAttachByName("M2.SharedDestroyBuffers", &hkSharedDestroyBuffers,
                              &g_origSharedDestroyBuffers))
        {
            WLOG_WARN("m2-indices: shared buffer teardown could not be reached; corrected-buffer"
                      " cache entries will persist until process exit");
        }
        if (installed)
            WLOG_INFO("m2-indices: 32-bit index starts honoured in instance and shared assembly");
        return installed;
    }
}
