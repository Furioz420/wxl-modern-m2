// Native modern-M2 reader: the split-skeleton (.skel) splice, including the derived-skeleton link.
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
//   Some modern models ship no bones and no sequences of their own. Their header carries the arrays
//   empty and the skeleton lives in a companion file named by the SKID chunk, shared between every
//   model built on it. Nothing downstream tolerates that: an unboned model animates to nothing and the
//   runtime's own initialize step tallies bone flags it would never find. This phase gives the header
//   back the arrays it is missing.
//
//   WHERE THE BYTES LIVE. The companion cannot be merged into the model's own load buffer. That buffer
//   is the client's allocation, sized to the model file, and its exact pointer is what the destructor
//   free and the model arena's bookkeeping expect to get back -- there is no room to grow it and no way
//   to swap it out. So the companion stays in an allocation of ours, kept whole, with the header's
//   arrays pointing straight into it. Ownership follows the module registry's rule exactly, and for the
//   same reason: the engine hands a freed model's address to the next one, so the pre-load event on an
//   address is what releases whatever the previous tenant left there.
//
//   WHY THE EXISTING WALK JUST WORKS. Every offset inside a companion's SKB1/SKS1/SKA1 payloads is
//   relative to that payload's own start, and no payload points into another. So each is a self-
//   contained little body, and the walk that resolves the model body resolves these too -- one call per
//   array, each against its own payload as base, reading the same record map the full-body walk reads.
//   Sequences go first because a bone's per-sequence track slots are keyed off the sequence flags.
//
//   THE DERIVED SKELETON. A companion may name a parent (SKPD), which is de-duplication: the derived
//   file stores the same bones in the same order as its parent, differing ONLY in where they sit, plus
//   a handful of race-specific sequences. It does not restate the animation set, which is why such a
//   file carries a few dozen sequences instead of a few hundred and ships no AFID of its own. So the
//   parent supplies everything that is indexed by sequence -- the sequence list, the global loops, the
//   attachments and every bone's tracks -- and the derived file supplies only the bind pose. Splitting
//   it on exactly that line is what keeps every index self-consistent: nothing is re-numbered, because
//   nothing is mixed within one array. What is deferred is the derived file's own sequences (a race's
//   distinctive idles); a derived model animates with its parent's set until they are layered in.

#include "M2NativeInternal.hpp"
#include "M2WalkLayout.hpp"
#include "NativeLoad.hpp"
#include "EventTrackSlots.hpp"

#include "../ExtensionApi.hpp"

#include "engine/assets/shared/models/m2/M2Format.hpp"
#include "game/Io.hpp"
#include "game/M2.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace fmt = wxl::structure::m2;
namespace io  = wxl::game::io;

namespace
{
    using wxl::runtime::m2native::detail::Rd32;
    using wxl::runtime::m2native::detail::FitsInBody;

    /// Companion chunk tags as the little-endian dword read of the on-disk bytes: the same convention
    /// the MD21 container itself uses, NOT byte-reversed.
    constexpr uint32_t Tag(const char (&s)[5])
    {
        return uint32_t(uint8_t(s[0])) | (uint32_t(uint8_t(s[1])) << 8) |
               (uint32_t(uint8_t(s[2])) << 16) | (uint32_t(uint8_t(s[3])) << 24);
    }

    /// Far above the largest companion in modern content (~1.4 MB) and far below anything that could
    /// exhaust the address space of a 32-bit process. A file past it is treated as malformed.
    constexpr uint32_t kMaxCompanionBytes = 64u * 1024u * 1024u;

    /// One payload chunk, as a body the walk can resolve against.
    struct Payload
    {
        uint8_t* base;
        uint32_t size;
    };

    /// One companion file: the allocation, the payloads that carry arrays, and its parent link.
    struct Companion
    {
        uint8_t* bytes;
        Payload  bones;
        Payload  sequences;
        Payload  attachments;
        uint32_t parent;
    };

    /// The header slots a companion fills, once resolved against that companion's own payloads.
    struct Resolved
    {
        fmt::M2Array globalLoops, sequences, sequenceLookup;
        fmt::M2Array bones, boneLookup;
        fmt::M2Array attachments, attachmentLookup;
        bool         hasAttachments;
    };

    /// Where each array head sits inside its payload. SKB1 and SKA1 open with their two array heads;
    /// SKS1 with three. SKPD holds its parent skeleton id after two reserved dwords.
    constexpr uint32_t kHeadFirst  = 0x00;
    constexpr uint32_t kHeadSecond = 0x08;
    constexpr uint32_t kHeadThird  = 0x10;
    constexpr uint32_t kParentIdAt = 0x08;

    /// One bone record: the bind-pose fields sit either side of the three animation tracks, so a bind
    /// pose transplant is those two spans and nothing between them.
    constexpr uint32_t kBoneStride    = 0x58;
    constexpr uint32_t kBoneBindHead  = 0x10; ///< key-bone id, flags, parent, submesh, name hash
    constexpr uint32_t kBonePivotAt   = 0x4C;
    constexpr uint32_t kBonePivotSize = 0x0C;

    /// The companion bytes a spliced model's header now points into, held for that model's lifetime.
    std::mutex g_mutex;
    std::unordered_map<void*, uint8_t*> g_owned;
    std::unordered_map<void*, wxl::runtime::m2native::detail::EventSlots> g_eventSlots;

    /**
     * ModelFilePath.db2 can resolve an M2 FileDataID, but Retail companion skeletons are not model
     * rows and therefore have no entry in that table. Derived playable skeletons name their parent
     * only by FileDataID in SKPD. Keep the small verified compatibility boundary here, beside the
     * consumer of SKPD, instead of pretending these are model IDs in the shared resolver.
     *
     * These eight IDs/paths were verified against the local PTR 12.1 listfile and the exact files in
     * the active Patch-Z payload. They cover the currently playable Lightforged, Void Elf,
     * Nightborne, and Dark Iron male/female actors.
     */
    const char* PlayableParentSkeletonPath(uint32_t fileDataId)
    {
        switch (fileDataId)
        {
            case 1685880: return "character\\draenei\\male\\draeneimale_hd.skel";
            case 1699074: return "character\\draenei\\female\\draeneifemale_hd.skel";
            case 1838505: return "character\\bloodelf\\female\\bloodelffemale_hd.skel";
            case 1838675: return "character\\bloodelf\\male\\bloodelfmale_hd.skel";
            case 1839627: return "character\\nightelf\\female\\nightelffemale_hd.skel";
            case 1839628: return "character\\nightelf\\male\\nightelfmale_hd.skel";
            case 1892673: return "character\\dwarf\\female\\dwarffemale_hd.skel";
            case 1892675: return "character\\dwarf\\male\\dwarfmale_hd.skel";
            default: return nullptr;
        }
    }

    /// Reads an array head out of a payload, or leaves it empty when the payload is too short to hold
    /// one -- an absent chunk and a chunk with nothing in it must reach the header the same way.
    fmt::M2Array HeadAt(const Payload& p, uint32_t at)
    {
        fmt::M2Array head{};
        if (p.base && at + sizeof head <= p.size) std::memcpy(&head, p.base + at, sizeof head);
        return head;
    }

    /// Names the companion beside the model: same path, extension replaced. A model reaches its own
    /// skin and .anim files the same way, so a companion that the client can open at all is reachable
    /// here without consulting any id table.
    bool CompanionPath(const char* stem, char* out, size_t cap)
    {
        if (!stem || !*stem) return false;
        const size_t n = std::strlen(stem);
        if (n + sizeof ".skel" > cap) return false;

        std::memcpy(out, stem, n);
        size_t cut = n;
        for (size_t i = n; i-- > 0;)
        {
            if (out[i] == '\\' || out[i] == '/') break;
            if (out[i] == '.') { cut = i; break; }
        }
        std::memcpy(out + cut, ".skel", sizeof ".skel");
        return true;
    }

    /// Whole companion file into one allocation of ours, then one chunk walk over it. Leaves c zeroed
    /// and returns false on any failure, so a caller can try a fallback without cleaning up first.
    bool LoadCompanion(const char* path, Companion& c)
    {
        std::memset(&c, 0, sizeof c);
        if (!path || !*path) return false;

        void* handle = nullptr;
        if (!io::FileOpen(path, io::kOpenWholeFile, &handle) || !handle) return false;

        uint32_t sizeHigh = 0;
        const uint32_t size = io::FileSize(handle, &sizeHigh);
        if (sizeHigh || size < 8 || size > kMaxCompanionBytes) { io::FileClose(handle); return false; }

        auto* bytes = static_cast<uint8_t*>(std::malloc(size));
        uint32_t read = 0;
        const bool ok = bytes && io::FileRead(handle, bytes, size, &read) && read == size;
        io::FileClose(handle);
        if (!ok) { std::free(bytes); return false; }

        for (uint32_t at = 0; at + 8 <= size;)
        {
            const uint32_t tag = Rd32(bytes + at);
            const uint32_t sz  = Rd32(bytes + at + 4);
            if (sz > size || at + 8 + sz > size) break; // malformed tail; keep what we have
            uint8_t* payload = bytes + at + 8;

            switch (tag)
            {
            case Tag("SKB1"): c.bones       = { payload, sz }; break;
            case Tag("SKS1"): c.sequences   = { payload, sz }; break;
            case Tag("SKA1"): c.attachments = { payload, sz }; break;
            case Tag("SKPD"):
                if (sz >= kParentIdAt + 4) c.parent = Rd32(payload + kParentIdAt);
                break;
            default: break;
            }
            at += 8 + sz;
        }

        // Bones are the whole point, and every per-sequence track slot inside one is keyed off the
        // sequence records: a file carrying either without the other cannot be resolved at all.
        if (!c.bones.base || !c.sequences.base) { std::free(bytes); std::memset(&c, 0, sizeof c); return false; }

        c.bytes = bytes;
        return true;
    }

    void FreeCompanion(Companion& c)
    {
        std::free(c.bytes);
        std::memset(&c, 0, sizeof c);
    }
}

namespace wxl::runtime::m2native::detail
{
    bool PadEventSlots(void* model, fmt::M2Header* h)
    {
        try
        {
            EventSlots slots;
            if (!PrepareEventSlots(h, slots)) return false;
            bool padded = false;
            for (const auto& list : slots) padded |= !list.empty();
            if (!padded) return true;
            std::lock_guard<std::mutex> lock(g_mutex);
            auto& owned = g_eventSlots[model];
            owned = std::move(slots);
            PublishEventSlots(h, owned);
            return true;
        }
        catch (...) { return false; }
    }
    namespace
    {
        /// Resolves one companion's three payloads into real pointers, each against its own payload as
        /// base, using the very same table entries the full-body walk uses for those arrays.
        bool ResolveCompanion(Companion& c, Outcome& out, Resolved& r)
        {
            // Sequences first: every per-sequence track slot below is keyed off their flags.
            fmt::M2Header seqStub{};
            seqStub.globalLoops    = HeadAt(c.sequences, kHeadFirst);
            seqStub.sequences      = HeadAt(c.sequences, kHeadSecond);
            seqStub.sequenceLookup = HeadAt(c.sequences, kHeadThird);
            // The same sequence deltas the model body gets: a companion's records come from the same
            // exporter and arrive in the same shape.
            FixSequencesRaw(c.sequences.base, c.sequences.size, &seqStub, out.extSeqPending);
            for (uint32_t at : { offsetof(fmt::M2Header, globalLoops),
                                 offsetof(fmt::M2Header, sequences),
                                 offsetof(fmt::M2Header, sequenceLookup) })
            {
                const HeaderArray* entry = HeaderArrayFor(at);
                if (!entry || !WalkOneArray(c.sequences.base, c.sequences.size, &seqStub, *entry,
                                             nullptr, 0))
                    return false;
            }
            const auto* seqRecords =
                reinterpret_cast<const fmt::M2Sequence*>(static_cast<uintptr_t>(seqStub.sequences.offset));

            fmt::M2Header boneStub{};
            boneStub.sequences  = seqStub.sequences; // resolved; the bone tracks read these flags
            boneStub.bones      = HeadAt(c.bones, kHeadFirst);
            boneStub.boneLookup = HeadAt(c.bones, kHeadSecond);
            for (uint32_t at : { offsetof(fmt::M2Header, bones), offsetof(fmt::M2Header, boneLookup) })
            {
                const HeaderArray* entry = HeaderArrayFor(at);
                if (!entry || !WalkOneArray(c.bones.base, c.bones.size, &boneStub, *entry, seqRecords,
                                             seqStub.sequences.count))
                    return false;
            }

            fmt::M2Header attachStub{};
            attachStub.sequences        = seqStub.sequences;
            attachStub.attachments      = HeadAt(c.attachments, kHeadFirst);
            attachStub.attachmentLookup = HeadAt(c.attachments, kHeadSecond);
            if (c.attachments.base)
            {
                for (uint32_t at : { offsetof(fmt::M2Header, attachments),
                                     offsetof(fmt::M2Header, attachmentLookup) })
                {
                    const HeaderArray* entry = HeaderArrayFor(at);
                    if (!entry || !WalkOneArray(c.attachments.base, c.attachments.size, &attachStub,
                                                *entry, seqRecords, seqStub.sequences.count))
                        return false;
                }
            }

            r.globalLoops      = seqStub.globalLoops;
            r.sequences        = seqStub.sequences;
            r.sequenceLookup   = seqStub.sequenceLookup;
            r.bones            = boneStub.bones;
            r.boneLookup       = boneStub.boneLookup;
            r.attachments      = attachStub.attachments;
            r.attachmentLookup = attachStub.attachmentLookup;
            r.hasAttachments   = c.attachments.base != nullptr;
            return true;
        }

        /**
         * @brief Writes a derived skeleton's bind pose onto the bone records of the skeleton it derives
         *        from, which by then hold the resolved animation tracks.
         *
         * The two bone arrays are the same bones in the same order, and the only thing a derived file
         * changes is where they sit, which is what gives the race its proportions. So the animating
         * records are kept whole, tracks and all, and only the two bind-pose spans either side of those
         * tracks are taken from the derived file. Refuses on any disagreement about the array itself,
         * because a mismatched count means the two files are not the pair they claim to be.
         * @param bind   the derived companion, still raw (only fixed-offset scalars are read).
         * @param bones  the resolved bone array of the skeleton being animated, rewritten in place.
         */
        bool StampBindPose(const Companion& bind, const fmt::M2Array& bones)
        {
            const fmt::M2Array head = HeadAt(bind.bones, kHeadFirst);
            if (!head.count || head.count != bones.count) return false;
            if (!FitsInBody(bind.bones.size, head.offset, head.count, kBoneStride)) return false;

            const uint8_t* src = bind.bones.base + head.offset;
            auto* dst = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(bones.offset));
            for (uint32_t i = 0; i < head.count; ++i, src += kBoneStride, dst += kBoneStride)
            {
                std::memcpy(dst, src, kBoneBindHead);
                std::memcpy(dst + kBonePivotAt, src + kBonePivotAt, kBonePivotSize);
            }
            return true;
        }
    }

    /**
     * @brief Fills a split-skeleton model's empty bone / sequence / attachment arrays from its
     *        companion .skel, following the parent link when the companion is a derived skeleton.
     *
     * Main thread only: it opens client files. All-or-nothing -- the header is written only once every
     * payload has resolved, so a rejected companion leaves an empty-armed model rather than a half-
     * armed one, and the caller can fail the load cleanly.
     * @param model  runtime model, for its path and for keying the companion allocation.
     * @param h      the model's header, arrays already pointer-based from the body walk.
     * @param out    receives what the companion supplied, for the stats and the load log.
     * @return true when the header now holds a complete skeleton.
     */
    bool SpliceSkeleton(void* model, fmt::M2Header* h, Outcome& out)
    {
        out.phase = 71; // resolve and open the model's companion
        char path[260];
        if (!CompanionPath(wxl::game::m2::M2Model(model).GetPathStem(), path, sizeof path)) return false;

        Companion child{};
        if (!LoadCompanion(path, child)) return false;
        out.skelParent = child.parent;

        // A derived skeleton animates with its parent's set. Only one hop is followed: no shipped
        // parent is itself derived, and a chain would mean this rule is not what the link means.
        Companion parent{};
        Companion* anim = &child;
        if (child.parent)
        {
            out.phase = 72; // resolve an optional parent companion
            // Prefer the verified companion-only map. ResolveModel remains a forward-compatible
            // fallback if the shared FDID service later gains a generic/listfile path source.
            const char* parentPath = PlayableParentSkeletonPath(child.parent);
            if (!parentPath) parentPath = wxl_modern_m2::ResolveModel(child.parent);
            if (LoadCompanion(parentPath, parent) &&
                HeadAt(parent.bones, kHeadFirst).count == HeadAt(child.bones, kHeadFirst).count)
            {
                anim = &parent;
                out.skelInherited = 1;
            }
            else
            {
                // Unresolved, or not the pair it claims to be. The derived file alone still stands the
                // model up on its own bind pose, with only the sequences it carries itself.
                WLOG_WARN("m2native: '%s' could not inherit parent skeleton %u from '%s'",
                          path, child.parent, parentPath ? parentPath : "<unresolved>");
                FreeCompanion(parent);
            }
        }

        out.phase = 73; // normalize and walk companion arrays
        Resolved r{};
        const bool ok = ResolveCompanion(*anim, out, r) &&
                        (anim == &child || StampBindPose(child, r.bones));
        if (!ok)
        {
            FreeCompanion(child);
            FreeCompanion(parent);
            return false;
        }

        out.phase = 74; // commit resolved companion arrays to the model
        // --- commit: nothing above this line touched the model ---
        // Only the arrays the companion actually carries. A chunk it does not ship must leave the
        // body's own array as the body walk resolved it, not blank it.
        h->globalLoops    = r.globalLoops;
        h->sequences      = r.sequences;
        h->sequenceLookup = r.sequenceLookup;
        h->bones          = r.bones;
        h->boneLookup     = r.boneLookup;
        if (r.hasAttachments)
        {
            h->attachments      = r.attachments;
            h->attachmentLookup = r.attachmentLookup;
        }

        out.skelBones     = r.bones.count;
        out.skelSequences = r.sequences.count;

        ReleaseSkeleton(model); // an address the engine handed us again still holds the last tenant's
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_owned[model] = anim->bytes;
        }
        // The bind-pose donor was read into the records above and is not pointed at any more.
        if (anim == &parent) FreeCompanion(child);
        return true;
    }
}

namespace wxl::runtime::m2native
{
    /**
     * @brief Releases the companion skeleton a model was spliced with, if it had one.
     *
     * Called from the pre-load event, on the same reasoning the module registry is cleared there: a
     * model address the engine hands back is a new model, and whatever the previous tenant's header
     * pointed into is dead the moment that happens.
     * @param model Runtime model pointer.
     */
    void ReleaseSkeleton(void* model)
    {
        uint8_t* bytes = nullptr;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_eventSlots.erase(model);
            auto it = g_owned.find(model);
            if (it == g_owned.end()) return;
            bytes = it->second;
            g_owned.erase(it);
        }
        std::free(bytes);
    }
}
