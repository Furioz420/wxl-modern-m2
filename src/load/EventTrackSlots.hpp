// Copyright (C) 2026 WarcraftXL. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "engine/assets/shared/models/m2/M2Format.hpp"
#include <vector>
#include <cstring>
#include <cstdint>

namespace wxl::runtime::m2native::detail
{
    // Native event dispatch indexes every nonempty per-sequence list without
    // checking its length. Unlike ordinary value tracks, short lists must be
    // extended before the model becomes visible to the animation workers.
    using EventSlots = std::vector<std::vector<wxl::structure::m2::M2Array>>;
    inline bool PrepareEventSlots(wxl::structure::m2::M2Header* h, EventSlots& storage)
    {
        using wxl::structure::m2::M2Array;
        storage.clear();
        if (!h || !h->events.count) return h != nullptr;
        if (!h->events.offset || !h->sequences.count || h->events.count > 4096 ||
            h->sequences.count > 65536) return false;
        auto* events = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(h->events.offset));
        uint64_t slots = 0;
        // Validate the complete plan before allocating or changing any header.
        for (uint32_t i = 0; i < h->events.count; ++i)
        {
            const auto* e = events + i * 36;
            int16_t global; M2Array list;
            std::memcpy(&global, e + 26, 2); std::memcpy(&list, e + 28, 8);
            if (global != -1 || !list.count) continue;
            if (!list.offset || list.count > h->sequences.count) return false;
            if (list.count < h->sequences.count) slots += h->sequences.count;
        }
        if (slots > 1024 * 1024) return false; // bound supplemental storage to 8 MiB
        storage.resize(h->events.count);
        for (uint32_t i = 0; i < h->events.count; ++i)
        {
            const auto* e = events + i * 36;
            int16_t global; M2Array list;
            std::memcpy(&global, e + 26, 2); std::memcpy(&list, e + 28, 8);
            if (global != -1 || !list.count || list.count == h->sequences.count) continue;
            storage[i].resize(h->sequences.count); // absent sequences have zero keys
            std::memcpy(storage[i].data(), reinterpret_cast<const void*>(static_cast<uintptr_t>(list.offset)),
                        list.count * sizeof(M2Array));
        }
        return true;
    }
    inline void PublishEventSlots(wxl::structure::m2::M2Header* h, const EventSlots& storage)
    {
        using wxl::structure::m2::M2Array;
        auto* events = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(h->events.offset));
        for (size_t i = 0; i < storage.size(); ++i)
            if (!storage[i].empty())
            {
                const M2Array list{static_cast<uint32_t>(storage[i].size()),
                    static_cast<uint32_t>(reinterpret_cast<uintptr_t>(storage[i].data()))};
                std::memcpy(events + i * 36 + 28, &list, sizeof(list));
            }
    }
}
