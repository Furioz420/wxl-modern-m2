// Copyright (C) 2026 WarcraftXL. GPL-3.0-or-later.
#pragma once
#include <d3d9.h>
#include <array>
namespace wxl::modern::particlelayers {
    // Only after Decode accepted the verified modern multitexture / source blend-7 subset.
    // Direct D3D state, never an extended index into the legacy engine's blend table.
    struct MaterialState { D3DRENDERSTATETYPE state; DWORD value; };
    inline constexpr std::array<MaterialState,7> BlendAddStates{{
        {D3DRS_ALPHABLENDENABLE,TRUE}, {D3DRS_SRCBLEND,D3DBLEND_ONE},
        {D3DRS_DESTBLEND,D3DBLEND_INVSRCALPHA}, {D3DRS_BLENDOP,D3DBLENDOP_ADD},
        {D3DRS_SEPARATEALPHABLENDENABLE,FALSE}, {D3DRS_ALPHATESTENABLE,FALSE},
        {D3DRS_ZWRITEENABLE,FALSE}
    }};
    template<class Device> HRESULT ApplyBlendAdd(Device* device, unsigned& failedStage) noexcept {
        for(unsigned n=0;n<BlendAddStates.size();++n) {
            const auto& s=BlendAddStates[n];
            const HRESULT hr=device->SetRenderState(s.state,s.value);
            if(FAILED(hr)){failedStage=n;return hr;}
        }
        failedStage=UINT_MAX;return S_OK;
    }
}
