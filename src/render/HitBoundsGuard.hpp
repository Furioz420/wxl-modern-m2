// Copyright (C) 2026 WarcraftXL. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>

namespace wxl::modern::hitbounds
{
    constexpr uintptr_t kSite = 0x0081D0A6;
    constexpr uint8_t kOriginal[]{0x8b,0x96,0x94,0,0,0,0x0f,0xb7,0x42,0x48,
        0x8b,0x4f,0x20,0xc1,0xe0,0x06,0x8d,0x7c,0x08,0x20,0xeb,0x06};
    // Tests point these at local continuations and execute this exact x86 thunk.
    inline uintptr_t continuation = 0x0081D0C2;
    inline uintptr_t staticBounds = 0x0081D0BC;

    // At this verified instruction boundary ESI is the instance, EDI the M2
    // header. The native type-3 path already uses header+0xBC static bounds.
    // Use that same path for absent/out-of-range animation, without changing
    // animation state or discarding the object from the hit list.
    __declspec(naked) inline void SelectBounds()
    {
        __asm
        {
            mov edx, [esi+094h]
            test edx, edx
            jz fallback
            movzx eax, word ptr [edx+048h]
            cmp eax, 0ffffh
            je fallback
            cmp eax, [edi+01ch]
            jae fallback
            mov ecx, [edi+020h]
            test ecx, ecx
            jz fallback
            shl eax, 6
            lea edi, [eax+ecx+020h]
            jmp dword ptr [continuation]
        fallback:
            jmp dword ptr [staticBounds]
        }
    }
}
