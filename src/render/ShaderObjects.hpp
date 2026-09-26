// Copyright (C) 2026 WarcraftXL. GPL-3.0-or-later.
#pragma once
#include <d3d9.h>
namespace wxl::modern::shaderobjects {
void Initialize();
// Only compiled-in immutable program arrays may be passed here.
HRESULT Vertex(IDirect3DDevice9*,const DWORD*,IDirect3DVertexShader9**);
HRESULT Pixel(IDirect3DDevice9*,const DWORD*,IDirect3DPixelShader9**);
}
