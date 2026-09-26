// Client-build details owned by wxl-modern-m2 rather than the shared Core SDK.
// These addresses support retail item synthesis and character presentation only.
// Copyright (C) 2026 WarcraftXL. GPLv3.

#pragma once

#include <cstddef>
#include <cstdint>

namespace wxl_modern_m2::private_offsets
{
    namespace db2
    {
        namespace item
        {
            constexpr uintptr_t kRecordCount = 0x00AD3D54;
            constexpr uintptr_t kRecordData  = 0x00AD3D68;
        }

        namespace itemdisplayinfo
        {
            constexpr uintptr_t kRecordCount = 0x00AD3DE4;
            constexpr uintptr_t kRecordData  = 0x00AD3DF8;
            constexpr size_t kOffIcon1       = 0x14;
            constexpr size_t kOffGroupSound  = 0x30;
            constexpr size_t kCompactRecordSize = 100;
        }
    }

    namespace m2
    {
        // Build 12340 IsLoggedIn (0x60A450) returns nil when this byte is nonzero.
        // Unlike terrain map ID, it changes when returning to Glue after logout.
        constexpr uintptr_t kNotLoggedIn = 0x00BD0793;
        // Native per-GUID invalidation: flags=2 marks the player portrait cache
        // row dirty and emits UNIT_PORTRAIT_UPDATE. cdecl, two stack arguments.
        constexpr uintptr_t kInvalidatePlayerPortrait = 0x00618110;
        using InvalidatePlayerPortraitFn = void(__cdecl*)(const uint64_t*, uint32_t);
        constexpr uintptr_t kCharModelApplyDisplay = 0x004F2830;
        constexpr uintptr_t kCharacterRemoveVisuals = 0x004EAF70;
        constexpr uintptr_t kCharacterModelFrameRemoveVisualsReturnA = 0x00597642;
        constexpr uintptr_t kCharacterModelFrameRemoveVisualsReturnB = 0x0059765B;
        constexpr uintptr_t kPortraitTextureRemoveVisualsReturn = 0x00619815;
        constexpr uintptr_t kGlueSelectCharacter = 0x004E3CD0;
        constexpr uintptr_t kCharacterCreationComponent = 0x00B6B1A0;
        constexpr uintptr_t kGlueSelectedCharacter = 0x00AC436C;
        constexpr uintptr_t kGlueCharacterCount = 0x00B6B23C;
        constexpr uintptr_t kGlueCharacterRecords = 0x00B6B240;
        constexpr size_t kGlueCharacterStride = 0x198;
        constexpr size_t kGlueDisplayArray = 0x50;
        constexpr uint32_t kGlueDisplayCount = 0x17;

        constexpr size_t kOffInstAttachedPrev = 0x5C;
        constexpr size_t kOffCmoHeadDisplay = 0x428;
        constexpr size_t kOffCmoChestDisplay = 0x434;
        constexpr size_t kOffCmoCapeDisplay = 0x450;
        constexpr uintptr_t kCharacterGeosRenderPrep = 0x004ED900;

        using ApplyDisplayFn = void(__fastcall*)(
            void* cmo, void* edx, uint32_t modelSlot, uint32_t displayId, uint32_t postFlag);
        using CharacterRemoveVisualsFn = void(__cdecl*)(void* instance);
        using GlueSelectCharacterFn = void(__cdecl*)();
        using CharacterGeosRenderPrepFn = void(__fastcall*)(void* cmo, void* edx);
    }

    namespace unit
    {
        constexpr uintptr_t kFieldSetWrite = 0x00743BAC;
        constexpr uint32_t kVisibleItemMainhandEntry = 0x139;
        constexpr uint32_t kVisibleItemOffhandEntry  = 0x13B;
        constexpr uint32_t kVisibleItemRangedEntry   = 0x13D;
        constexpr size_t kFieldArrayOffset = 0x1958;

        using FieldSetWriteFn = void(__cdecl*)();
    }
}
