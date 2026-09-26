// M2 buffer allocator: routes large model buffers into the core-owned arena (wxl.m2arena), with a
// standalone-VirtualAlloc fallback.
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

// The arena RESERVATION itself lives in core (src/client/CM2Shared/Memory.cpp, Boot phase -- see its
// own comment for why). This file only owns the DECISION of which allocations to route there: past a
// size threshold, ask wxl.m2arena for a range; if the arena is unavailable/exhausted/failed, fall back
// to a standalone VirtualAlloc. Below the native threshold, defer to the client's own allocator.

#include "../ExtensionApi.hpp"
#include "wxl/EventScript.hpp"
#include <memory>
#include "../compat/SourceMaterialCapture.hpp"
#include "client/CharModel/RetailSkinProvider.hpp"

#include "offsets/game/M2.hpp"

#include <windows.h>

#include <cstdint>
#include <mutex>
#include <unordered_map>
#include "NativeAllocationStats.hpp"

namespace
{
    namespace m2 = wxl::offsets::game::m2;

    m2::M2_BufferAllocFn g_origM2BufferAlloc = nullptr;
    m2::M2_BufferFreeFn  g_origM2BufferFree  = nullptr;

    constexpr uint32_t kVirtualM2AllocThreshold = 1u * 1024u * 1024u;

    struct VirtualM2Allocation
    {
        void* base = nullptr;      // non-null for standalone VirtualAlloc
        uint32_t arenaOffset = 0;  // valid when base == nullptr
        uint32_t arenaSize = 0;
        uint32_t requestedSize = 0;
    };

    std::mutex g_virtualM2AllocMutex;
    std::unordered_map<void*, VirtualM2Allocation> g_virtualM2Allocs;

    bool g_traceNative = false;
    std::mutex g_nativeStatsMutex;
    wxl_memory::NativeAllocationStats<131072> g_nativeStats;

    void* TryVirtualM2Alloc(uint32_t size)
    {
        const SIZE_T total = static_cast<SIZE_T>(size) + 0x20u;
        auto* base = static_cast<uint8_t*>(VirtualAlloc(nullptr, total, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (!base)
            return nullptr;

        const uintptr_t aligned = (reinterpret_cast<uintptr_t>(base) + 0x1Fu) & ~uintptr_t(0x0Fu);
        auto* ptr = reinterpret_cast<uint8_t*>(aligned);
        const uintptr_t shift = reinterpret_cast<uintptr_t>(ptr) - reinterpret_cast<uintptr_t>(base);
        if (shift == 0 || shift > 0xFF)
        {
            VirtualFree(base, 0, MEM_RELEASE);
            return nullptr;
        }
        ptr[-1] = static_cast<uint8_t>(shift);

        std::lock_guard<std::mutex> lock(g_virtualM2AllocMutex);
        g_virtualM2Allocs.emplace(ptr, VirtualM2Allocation{ base, 0, 0, size });
        return ptr;
    }

    void __cdecl hkM2BufferFree(void* ptr)
    {
        if (!ptr)
            return;

        // Raw MD21 bodies retain this exact allocation address through normalization.
        // Forget before either native free, arena decommit, or VirtualFree can reuse it.
        wxl::modern::assets::m2::material::SourceMaterials().Forget(ptr);

        VirtualM2Allocation alloc{};
        bool ours = false;
        {
            std::lock_guard<std::mutex> lock(g_virtualM2AllocMutex);
            auto it = g_virtualM2Allocs.find(ptr);
            if (it != g_virtualM2Allocs.end())
            {
                alloc = it->second;
                g_virtualM2Allocs.erase(it);
                ours = true;
            }
        }

        if (ours && !alloc.base)
        {
            if (const WXL_M2ArenaApi* arena = wxl_modern_m2::Arena())
                arena->Free(alloc.arenaOffset, alloc.arenaSize);
            return;
        }

        if (ours && alloc.base)
        {
            VirtualFree(alloc.base, 0, MEM_RELEASE);
            return;
        }

        if (g_traceNative) {
            std::lock_guard lock(g_nativeStatsMutex);
            g_nativeStats.Remove(reinterpret_cast<uintptr_t>(ptr));
        }
        g_origM2BufferFree(ptr);
    }

    void* __cdecl hkM2BufferAlloc(uint32_t size, const char* tag, int line)
    {
        if (size >= kVirtualM2AllocThreshold)
        {
            if (const WXL_M2ArenaApi* arena = wxl_modern_m2::Arena())
            {
                uint32_t offset = 0, allocSize = 0;
                if (void* ptr = arena->Alloc(size, &offset, &allocSize))
                {
                    {
                        std::lock_guard<std::mutex> lock(g_virtualM2AllocMutex);
                        g_virtualM2Allocs.emplace(ptr, VirtualM2Allocation{ nullptr, offset, allocSize, size });
                    }
                    WLOG_DEBUG("m2-memory: arena buffer %u bytes (%s)", size, tag ? tag : "M2");
                    if (size >= 8u * 1024u * 1024u) arena->LogAddressSpace("m2-arena");
                    return ptr;
                }
            }

            if (void* standalone = TryVirtualM2Alloc(size))
            {
                WLOG_DEBUG("m2-memory: virtual buffer %u bytes (%s)", size, tag ? tag : "M2");
                if (size >= 8u * 1024u * 1024u)
                    if (const WXL_M2ArenaApi* arena = wxl_modern_m2::Arena())
                        arena->LogAddressSpace("m2-virtual");
                return standalone;
            }
            if (size >= 8u * 1024u * 1024u)
                if (const WXL_M2ArenaApi* arena = wxl_modern_m2::Arena())
                    arena->LogAddressSpace("m2-virtual-failed");
            WLOG_WARN("m2-memory: VirtualAlloc failed for %u bytes, falling back to native allocator", size);
        }

        void* ptr = g_origM2BufferAlloc(size, tag, line);
        if (g_traceNative && ptr) {
            std::lock_guard lock(g_nativeStatsMutex);
            g_nativeStats.Add(reinterpret_cast<uintptr_t>(ptr), size);
        }
        return ptr;
    }
}

namespace wxl_modern_m2
{
    class MemoryTrace final : public wxl::ext::EventScript {
        DWORD next_ = 0;
    public:
        MemoryTrace() { on<&MemoryTrace::Tick>(wxl::events::Event::OnUpdate); }
        void Tick(const wxl::events::UpdateArgs&) {
            const DWORD now = GetTickCount();
            if (next_ && static_cast<int32_t>(now - next_) < 0) return;
            next_ = now + 1000;
            if (const auto* arena = Arena()) arena->LogAddressSpace("periodic-v1");
            uint64_t arenaBytes = 0, virtualBytes = 0;
            size_t arenaCount = 0, virtualCount = 0;
            {
                std::lock_guard lock(g_virtualM2AllocMutex);
                for (const auto& entry : g_virtualM2Allocs)
                {
                    if (entry.second.base)
                    {
                        virtualBytes += entry.second.requestedSize;
                        ++virtualCount;
                    }
                    else
                    {
                        arenaBytes += entry.second.requestedSize;
                        ++arenaCount;
                    }
                }
            }
            uint64_t nativeBytes = 0, missed = 0;
            size_t nativeCount = 0;
            {
                std::lock_guard lock(g_nativeStatsMutex);
                nativeBytes = g_nativeStats.bytes;
                nativeCount = g_nativeStats.count;
                missed = g_nativeStats.missed;
            }
            WLOG_INFO("m2-native-memory-v1: live=%u requested_mb=%.1f missed=%llu",
                static_cast<unsigned>(nativeCount), nativeBytes / (1024.0 * 1024.0),
                static_cast<unsigned long long>(missed));
            // Do not nest allocator/provider locks or log while either lock is held.
            const auto skins = wxl::client::charmodel::GetRetailSkinMemoryStats();
            constexpr double mib = 1024.0 * 1024.0;
            WLOG_INFO("memory-owners-v1: m2_arena_live=%u m2_arena_requested_mb=%.1f "
                      "m2_virtual_live=%u m2_virtual_requested_mb=%.1f "
                      "skin_files=%u skin_capacity_mb=%.1f component_files=%u "
                      "component_capacity_mb=%.1f prepared_paths=%u component_aliases=%u",
                      static_cast<unsigned>(arenaCount), arenaBytes / mib,
                      static_cast<unsigned>(virtualCount), virtualBytes / mib,
                      static_cast<unsigned>(skins.virtualFiles), skins.virtualBytes / mib,
                      static_cast<unsigned>(skins.componentFiles), skins.componentBytes / mib,
                      static_cast<unsigned>(skins.preparedPaths),
                      static_cast<unsigned>(skins.componentAliases));
        }
    };
    std::unique_ptr<MemoryTrace> memoryTrace;

    bool InstallM2Memory()
    {
        g_traceNative = ConfigBool("WXL_M2_MEMORY_TRACE", false);
        if (g_traceNative) {
            memoryTrace = std::make_unique<MemoryTrace>();
            WLOG_INFO("memory-trace-v2: periodic address-space and owner sampling enabled (1000ms)");
        }
        HookAttachByName("M2.BufferAlloc", &hkM2BufferAlloc, &g_origM2BufferAlloc);
        HookAttachByName("M2.BufferFree", &hkM2BufferFree, &g_origM2BufferFree);
        return true;
    }
}
