// Native modern-M2 reader: our own offset->pointer walk over the model body.
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

// The body is parsed in place: every count+offset pair in it is rewritten into a real pointer, so
// after the walk the runtime can chase the model without ever consulting the file layout again.
//
// Two invariants make this safe and reversible:
//  - an array only becomes a pointer once count * stride is proven to fit inside the body, so a
//    truncated or hostile file fails the walk instead of leaving wild pointers behind;
//  - a track slot belonging to a sequence whose keyframes live in a companion file is left
//    file-relative, because it is that file's arrival -- not this walk -- that gives it a base.

#include "M2NativeInternal.hpp"
#include "M2WalkLayout.hpp"

#include <cstdint>

namespace fmt = wxl::structure::m2;

namespace wxl::runtime::m2native::detail
{
    namespace
    {
        constexpr uint32_t kSeqKeysInline    = 0x20; ///< keyframes live in this file, not a companion one
        constexpr uint32_t kSeqPlaysOnce     = 0x01; ///< runs to its end instead of looping
        constexpr uint32_t kSeqPlayOnceReady = 0x80; ///< load-time twin of kSeqPlaysOnce
        constexpr int16_t  kNoGlobalLoop     = -1;   ///< track is keyed per sequence, not to a global loop

        /// Sequences that must loop whatever their record asks for -- the movement/idle set the pose
        /// system expects to be able to hold indefinitely.
        constexpr uint16_t kAlwaysLoopingIds[] = {
            0x00, 0x04, 0x05, 0x0D, 0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x45, 0x77, 0x78, 0x8F, 0xDF,
            // Sustained advanced-flight poses; the nearby takeoff/flap
            // sequences are transitions and intentionally remain one-shot.
            1524, 1530, 1532, 1704, 1722,
        };

        bool AlwaysLoops(uint16_t id)
        {
            for (uint16_t looping : kAlwaysLoopingIds)
                if (looping == id) return true;
            return false;
        }

        /// Turns one count+offset pair into a pointer, rejecting anything that does not fit the body.
        /// An empty array resolves to null so a consumer cannot walk stale bytes.
        bool Resolve(uint8_t* base, uint32_t size, fmt::M2Array& array, uint32_t stride)
        {
            const uint32_t offset = array.offset;
            if (!FitsInBody(size, offset, array.count, stride)) return false;
            array.offset = array.count
                ? static_cast<uint32_t>(reinterpret_cast<uintptr_t>(base) + offset) : 0;
            return true;
        }

        /// An array of arrays: the outer slot list plus every inner list, each bounds-checked.
        bool ResolveNested(uint8_t* base, uint32_t size, fmt::M2Array& outer, uint32_t innerStride)
        {
            if (!Resolve(base, size, outer, sizeof(fmt::M2Array))) return false;
            if (!outer.count) return true;
            auto* slot = reinterpret_cast<fmt::M2Array*>(static_cast<uintptr_t>(outer.offset));
            for (uint32_t i = 0; i < outer.count; ++i)
                if (!Resolve(base, size, slot[i], innerStride)) return false;
            return true;
        }

        /// True when an unresolved nested array is exactly one body-local key. This is the only
        /// per-sequence shape that can be classified without a sequence table: one timestamp/value
        /// pair describes a constant for every sequence, while any larger outer list could contain
        /// companion-relative offsets that must not be rebased against the model body.
        bool IsBodyLocalConstant(uint8_t* base, uint32_t size, const fmt::M2Array& outer,
                                 uint32_t innerStride, bool requireZeroTimestamp)
        {
            if (outer.count != 1 ||
                !FitsInBody(size, outer.offset, 1, sizeof(fmt::M2Array)))
                return false;

            const auto* const inner = reinterpret_cast<const fmt::M2Array*>(base + outer.offset);
            if (inner->count != 1 || !FitsInBody(size, inner->offset, 1, innerStride)) return false;
            return !requireZeroTimestamp || Rd32(base + inner->offset) == 0;
        }

        /// A per-sequence slot list: one slot per sequence, and only the slots whose sequence keeps its
        /// keyframes in this file get a base now. The rest stay file-relative for their own arrival.
        bool ResolvePerSequence(uint8_t* base, uint32_t size, fmt::M2Array& outer,
                                uint32_t innerStride, uint32_t sequenceCount,
                                const fmt::M2Sequence* sequences)
        {
            if (!Resolve(base, size, outer, sizeof(fmt::M2Array))) return false;
            if (!outer.count) return true;

            if (outer.count > sequenceCount) return false;

            auto* slot = reinterpret_cast<fmt::M2Array*>(static_cast<uintptr_t>(outer.offset));
            for (uint32_t i = 0; i < outer.count; ++i)
            {
                if (!(sequences[i].flags & kSeqKeysInline)) continue;
                if (!Resolve(base, size, slot[i], innerStride)) return false;
            }
            return true;
        }

        /// One animation track. Both halves of a per-sequence track are indexed by the SAME slot count
        /// (the timestamp list's) -- the two lists describe one key set, so they are walked in lockstep.
        bool ResolveTrack(uint8_t* base, uint32_t size, uint8_t* at, uint32_t valueStride,
                          const fmt::M2Sequence* sequences, uint32_t sequenceCount)
        {
            auto* track = reinterpret_cast<fmt::M2TrackHeader*>(at);
            if (track->globalSequence == kNoGlobalLoop)
            {
                // Split-skeleton bodies have no local sequence table yet; the companion .skel is
                // spliced only after this walk. Preserve a paired, provably body-local static key
                // (the modern representation used by constant texture weights), but keep dropping
                // every ambiguous slot list exactly as before. Resolve the two halves together: a
                // timestamp without its value, or vice versa, is not a valid track.
                if (!sequences || !sequenceCount)
                {
                    const bool localConstant = track->interpolationType == 0 && valueStride &&
                        IsBodyLocalConstant(base, size, track->timestamps, sizeof(uint32_t), true) &&
                        IsBodyLocalConstant(base, size, track->values, valueStride, false);
                    if (localConstant)
                    {
                        if (!ResolveNested(base, size, track->timestamps, sizeof(uint32_t)))
                            return false;
                        return ResolveNested(base, size, track->values, valueStride);
                    }

                    track->timestamps.count = 0;
                    track->timestamps.offset = 0;
                    // Timestamp-only event tracks end here. There is no values array;
                    // writing one would erase the next event's identifier and data.
                    if (valueStride)
                    {
                        track->values.count = 0;
                        track->values.offset = 0;
                    }
                    return true;
                }

                if (!ResolvePerSequence(base, size, track->timestamps, sizeof(uint32_t),
                                        sequenceCount, sequences))
                    return false;
                if (valueStride &&
                    !ResolvePerSequence(base, size, track->values, valueStride,
                                        sequenceCount, sequences))
                    return false;
                return true;
            }
            if (!ResolveNested(base, size, track->timestamps, sizeof(uint32_t))) return false;
            return !valueStride || ResolveNested(base, size, track->values, valueStride);
        }

        /// Applies one header entry's record map to every record it holds.
        bool ResolveRecords(uint8_t* base, uint32_t size, const fmt::M2Array& array, uint32_t stride,
                            const HeaderArray& entry, const fmt::M2Sequence* sequences,
                            uint32_t sequenceCount)
        {
            if (!array.count) return true;
            auto* record = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(array.offset));
            for (uint32_t i = 0; i < array.count; ++i, record += stride)
                for (uint32_t s = 0; s < entry.stepCount; ++s)
                {
                    const RecordStep& step = entry.steps[s];
                    uint8_t* at = record + step.at;
                    if (step.kind == kStepArray)
                    {
                        if (!Resolve(base, size, *reinterpret_cast<fmt::M2Array*>(at), step.stride))
                            return false;
                    }
                    else if (!ResolveTrack(base, size, at, step.stride, sequences, sequenceCount))
                    {
                        return false;
                    }
                }
            return true;
        }

        /// Sequence flag pass: a play-once record raises its load-time twin bit, and the ids that must
        /// stay loopable have the play-once bit taken back off.
        void ApplySequenceFlags(const fmt::M2Array& array)
        {
            if (!array.count) return;
            auto* sequences = reinterpret_cast<fmt::M2Sequence*>(static_cast<uintptr_t>(array.offset));
            for (uint32_t i = 0; i < array.count; ++i)
            {
                uint32_t flags = sequences[i].flags;
                if (AlwaysLoops(sequences[i].id))
                {
                    // Both bits participate in the client's one-shot path.
                    // Leaving the runtime-ready twin set freezes sustained
                    // stand/accessory animations on their final frame.
                    flags &= ~(kSeqPlaysOnce | kSeqPlayOnceReady);
                }
                else if (flags & kSeqPlaysOnce)
                    flags |= kSeqPlayOnceReady;
                sequences[i].flags = flags;
            }
        }
    }

    bool WalkOneArray(uint8_t* base, uint32_t size, fmt::M2Header* h,
                      const HeaderArray& entry, const fmt::M2Sequence* sequences,
                      uint32_t sequenceCount)
    {
        auto& array = *reinterpret_cast<fmt::M2Array*>(h->base() + entry.at);
        if (!Resolve(base, size, array, entry.stride)) return false;
        if (entry.traits & kArraySequenceFlags) ApplySequenceFlags(array);
        if (!entry.steps) return true;
        return ResolveRecords(base, size, array, entry.stride, entry, sequences, sequenceCount);
    }

    bool WalkDeferredEvents(uint8_t* base, uint32_t size, fmt::M2Header* h, const Outcome& out)
    {
        if (!out.needsSkel) return true;
        if (!h->sequences.count || !h->sequences.offset) return false;
        const auto* sequences = reinterpret_cast<const fmt::M2Sequence*>(
            static_cast<uintptr_t>(h->sequences.offset));
        for (const HeaderArray& entry : kHeaderArrays)
            if (entry.at == offsetof(fmt::M2Header, events))
                return WalkOneArray(base, size, h, entry, sequences, h->sequences.count);
        return false;
    }

    /**
     * @brief Resolves every array in the header, and every array nested in the records they reach.
     * @param base  body bytes; the pointers written point back into them.
     * @param size  body byte size, the bound every array is checked against.
     * @param h     header sitting at the body base.
     * @return true when every array fit the body; false leaves the walk abandoned mid-way, which is
     *         why the caller must treat a false as a failed load rather than a partial model.
     */
    bool WalkHeaderArrays(uint8_t* base, uint32_t size, fmt::M2Header* h, Outcome& out)
    {
        uint32_t index = 0;
        for (const HeaderArray& entry : kHeaderArrays)
        {
            out.phase = 500 + index++;
            // Body events use the companion's sequence indices. Keep their raw offsets
            // until the skeleton is spliced, rather than discarding their timestamps.
            if (out.needsSkel && entry.at == offsetof(fmt::M2Header, events)) continue;
            if ((entry.traits & kArrayCombinerGated) &&
                !(h->globalFlags & fmt::kFlagUseTextureCombinerCombos))
                continue;

            // Sequences resolve first in the table, so every later record can read them to decide
            // which of its track slots this file actually carries.
            const auto* sequences =
                reinterpret_cast<const fmt::M2Sequence*>(static_cast<uintptr_t>(h->sequences.offset));
            if (!WalkOneArray(base, size, h, entry, sequences, h->sequences.count)) return false;
        }
        return true;
    }
}
