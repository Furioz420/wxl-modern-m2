// Copyright (C) 2026 WarcraftXL. GPL-3.0-or-later.
// Diagnostic only: fingerprints managed DXT backing bytes, NOT sampled GPU pixels.
#pragma once
#include <d3d9.h>
#include <cstdint>
#include <cstddef>
namespace wxl::modern::textureaudit {
struct Layout { unsigned rowBytes=0, rows=0, bytes=0; };
inline bool Describe(unsigned width,unsigned height,D3DFORMAT format,Layout& out) noexcept {
    if(!width||!height||width>4096||height>4096)return false;
    const unsigned block=format==D3DFMT_DXT1?8u:(format==D3DFMT_DXT3||format==D3DFMT_DXT5)?16u:0u;
    if(!block)return false;
    Layout value{((width+3)/4)*block,(height+3)/4,0};value.bytes=value.rowBytes*value.rows;
    if(value.bytes>4u*1024*1024)return false;
    out=value;return true;
}
inline bool Fingerprint(const void* data,int pitch,const Layout& layout,uint64_t& out) noexcept {
    if(!data||!layout.rowBytes||!layout.rows||pitch<0||unsigned(pitch)<layout.rowBytes||
        uint64_t(unsigned(pitch))*layout.rows>16u*1024*1024)return false;
    uint64_t hash=14695981039346656037ull;
    for(unsigned row=0;row<layout.rows;++row) {
        const auto* bytes=static_cast<const uint8_t*>(data)+size_t(row)*unsigned(pitch);
        for(unsigned i=0;i<layout.rowBytes;++i){hash^=bytes[i];hash*=1099511628211ull;}
    }
    out=hash;return true;
}
struct Result {
    D3DSURFACE_DESC desc{};Layout layout{};uint64_t hash=0;
    HRESULT hr=E_FAIL;bool complete=false,unlockFailed=false;const char* reason="null-texture";
};
template<class Texture> Result Read(Texture* texture) noexcept {
    Result out;if(!texture)return out;
    out.reason="description";out.hr=texture->GetLevelDesc(0,&out.desc);if(FAILED(out.hr))return out;
    out.reason="unsupported-pool-format-or-size";
    if(out.desc.Pool!=D3DPOOL_MANAGED||!Describe(out.desc.Width,out.desc.Height,out.desc.Format,out.layout)){out.hr=S_FALSE;return out;}
    D3DLOCKED_RECT lock{};out.reason="read-only-lock";
    out.hr=texture->LockRect(0,&lock,nullptr,D3DLOCK_READONLY);if(FAILED(out.hr))return out;
    const bool valid=Fingerprint(lock.pBits,lock.Pitch,out.layout,out.hash);
    out.hr=texture->UnlockRect(0);out.unlockFailed=FAILED(out.hr);
    out.reason=out.unlockFailed?"unlock-failed":!valid?"invalid-pitch":"complete";
    out.complete=valid&&!out.unlockFailed;return out;
}
// Restore upload side effects before inheriting the native draw state; caller keeps
// the capture alive for post-draw/failure restoration as well.
template<class StateBlock> bool RestoreAfterReadiness(StateBlock* state) noexcept {
    return state&&SUCCEEDED(state->Apply());
}
}
