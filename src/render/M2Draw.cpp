// M2 batch draw path: owns the DrawIndexedPrimitive vtable slot, publishes wxl.m2draw for other
// extensions, folds ribbon multi-texture, re-expands 32-bit M2 start indices, publishes OnM2BatchDraw.
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

// wxl-modern-m2 is the sole owner of the DrawIndexedPrimitive vtable slot: core's Render.cpp swaps
// EndScene/Present/Reset but deliberately leaves this one alone (see its own top comment), and any
// other extension that needs to bracket one native draw (wxl-wmo's four-layer material) goes through
// the one-shot interceptor published here as "wxl.m2draw" (include/wxl/M2DrawApi.h) instead of
// touching the vtable itself. The swap is re-applied on the OnWorldRenderEnd cadence core already uses
// for its own three slots, so a device recreate (not a Reset -- the vtable survives that) is covered.

#include "../ExtensionApi.hpp"
#include "../compat/ModernM2.hpp"
#include "MaterialDiagnostics.hpp"
#include "MaterialBlendExperiment.hpp"
#include "ParticleDiagnostics.hpp"
#include "ParticleLayers.hpp"
#include "ParticleDrawTrace.hpp"
#include "RibbonShader.hpp"
#include "ShaderObjects.hpp"
#include "ShadowSpace.hpp"
#include "ShadowReceiverCapture.hpp"
#include "TerrainShadows.hpp"

#include "common/Mem.hpp"
#include "engine/events/Event.hpp"
#include "engine/assets/shared/models/m2/M2Format.hpp"
#include "game/Gx.hpp"
#include "game/M2.hpp"
#include "offsets/engine/Gx.hpp"
#include "offsets/game/M2.hpp"

#include <windows.h>
#include <d3d9.h>

#include <cstring>
#include <array>
#include <intrin.h>

namespace
{
    namespace off   = wxl::offsets::engine::gx;
    namespace ev    = wxl::events;
    namespace m2off = wxl::offsets::game::m2;
    namespace gx    = wxl::game::gx;

    // The model currently drawing, captured between a batch-draw enter and its DrawIndexedPrimitive.
    void* g_curModel = nullptr;
    // The current M2 batch draw context; its copied skin-section pointer is populated before the DIP call.
    void* g_curDrawCtx = nullptr;
    // Re-entrancy guard: a subscriber re-issues the draw through the hooked vtable, so do not re-emit.
    bool  g_inM2Emit = false;

    using DrawBatchFn = void (__fastcall*)(void* ctx, void* edx);
    DrawBatchFn g_origDrawBatch = nullptr;

    // The stock engine draw dispatcher divides the current vertex-buffer byte size by its stride at
    // 0x006A366B without checking for zero. A partially initialized modern/fallback model can leave a
    // wrapper bound with stride 0; skip that one malformed draw instead of terminating the client.
    off::GxDeviceDrawFn g_origDeviceDraw = nullptr;
    LONG                g_zeroStrideDrawSkips = 0;

    // Ribbon multi-texture: set true around a >= 3 layer ribbon's single native pass so the DIP override
    // folds its bound layers into one combine. The native ribbon draw is hooked separately below.
    m2off::M2_RibbonDrawFn g_origRibbonDraw = nullptr;
    bool                   g_ribbonModern   = false;

    // --- the DIP vtable slot itself ---------------------------------------------------------------
    WXL_M2Draw_DIPFn       g_origDIP      = nullptr;
    long __stdcall DrawNativeWithShadows(void* dev,int pt,int bv,unsigned mi,unsigned nv,unsigned si,unsigned pc)
    {
        // Most indexed calls in dense scenes are caster work. Avoid receiver lookup on that path.
        if(wxl::runtime::m2shadow::HasShadowContext()) return g_origDIP(dev,pt,bv,mi,nv,si,pc);
        return wxl::modern::terrainshadow::Draw(dev,pt,bv,mi,nv,si,pc,g_origDIP);
    }
    WXL_M2Draw_InterceptFn g_oneShot      = nullptr; // armed by wxl.m2draw's SetOneShotIntercept
    void EnsureDIPHook(IDirect3DDevice9* dev);   // defined after the detours

    // Default-off trace: eight whole Present intervals, spaced >=500ms, only after
    // exact-target activity. Fixed CPU storage; COM references never survive a snapshot.
    namespace pl=wxl::modern::particlelayers;
    namespace dt=wxl::modern::drawtrace;
    struct TraceRow {
        dt::Snapshot gpu;
        void* device=nullptr;void* instance=nullptr;void* shared=nullptr;void* ribbon=nullptr;
        void* caller=nullptr;
        char path[276]{};
        unsigned kind=0,engine=0,dips=0,context=0,target=0,route=0,vertices=0,primitives=0;
        int type=0,indexed=0,userMemory=-1;
    };
    std::array<TraceRow,2048> traceRows;
    dt::Schedule traceSchedule;
    unsigned traceCount=0,traceDropped=0,traceFrame=0,traceEngine=0,traceEngineRow=UINT_MAX;
    bool traceCapture=false,traceTargetSeen=false;
    void* traceRibbon=nullptr;
    void TraceModel(TraceRow& row) noexcept {
        row.instance=pl::TraceInstance();row.context=row.instance?1:0;
        if(!row.instance&&g_curModel){row.instance=g_curModel;row.context=2;}
        row.ribbon=traceRibbon;if(!row.context&&traceRibbon)row.context=3;
        __try {
            if(!row.instance)return;
            row.shared=*reinterpret_cast<void**>(static_cast<uint8_t*>(row.instance)+m2off::kOffInstModel);
            if(!row.shared)return;
            const char* path=wxl::game::m2::M2Model(row.shared).GetPathStem();
            if(!path)return;
            size_t n=0;for(;n+1<sizeof(row.path)&&path[n];++n)row.path[n]=path[n];
            if(path[n])strcpy_s(row.path,"<truncated>");
        }__except(EXCEPTION_EXECUTE_HANDLER){strcpy_s(row.path,"<unreadable>");}
    }
    unsigned TraceDraw(void* device,void* caller,unsigned kind,int type,unsigned vertices,unsigned primitives,int indexed) {
        if(!pl::TraceEnabled())return UINT_MAX;
        if(pl::TraceTarget())traceTargetSeen=true;
        if(!traceCapture)return UINT_MAX;
        if(traceCount==traceRows.size()){++traceDropped;return UINT_MAX;}
        const unsigned index=traceCount++;auto& row=traceRows[index];row=TraceRow{};
        row.device=device;row.caller=caller;row.kind=kind;row.type=type;row.vertices=vertices;row.primitives=primitives;
        row.indexed=indexed;row.engine=traceEngine;row.target=unsigned(pl::TraceTarget());
        row.route=g_oneShot?1:g_ribbonModern?2:pl::Active()?3:0;
        TraceModel(row);
        row.gpu=dt::Read(static_cast<IDirect3DDevice9*>(device));
        return index;
    }
    struct TraceEngineScope {
        unsigned previous=traceEngine,previousRow=traceEngineRow;
        TraceEngineScope(void* engineDevice,int* primitive,int indexed,void* caller) {
            if(!pl::TraceEnabled()||!traceCapture)return;
            traceEngine=traceCount+1;int type=-1,count=0,user=-1;
            __try {
                if(primitive){type=primitive[0];count=primitive[2];}
                if(engineDevice)user=*reinterpret_cast<int*>(static_cast<uint8_t*>(engineDevice)+off::kDeviceDrawUserMemory);
            }__except(EXCEPTION_EXECUTE_HANDLER){}
            traceEngineRow=TraceDraw(gx::RawDevice(),caller,0,type,0,count<0?0u:unsigned(count),indexed);
            if(traceEngineRow!=UINT_MAX)traceRows[traceEngineRow].userMemory=user;
        }
        ~TraceEngineScope(){traceEngine=previous;traceEngineRow=previousRow;}
    };
    void __cdecl TraceFrame(void*,const void* args) {
        if(!pl::TraceEnabled())return;
        const auto* frame=static_cast<const ev::FrameArgs*>(args);
        if(traceCapture) {
            const auto end=dt::Read(frame?static_cast<IDirect3DDevice9*>(frame->device):nullptr);
            WLOG_INFO("m2-draw-trace: frame=%u packet=%u rows=%u dropped=%u targetSeen=%u presentDevice=%p presentRT=%p backbuffer=%p valid=%#x",traceFrame,traceSchedule.used,traceCount,traceDropped,unsigned(traceTargetSeen),frame?frame->device:nullptr,reinterpret_cast<void*>(end.rt),reinterpret_cast<void*>(end.backbuffer),end.valid);
            for(unsigned n=0;n<traceCount;++n){const auto& r=traceRows[n];const auto& s=r.gpu;
                WLOG_INFO("m2-draw-trace: frame=%u row=%u kind=%u engine=%u dips=%u context=%u target=%u route=%u device=%p caller=%p instance=%p shared=%p ribbon=%p path='%s' type=%d vertices=%u primitives=%u indexed=%d userMemory=%d",traceFrame,n,r.kind,r.engine,r.dips,r.context,r.target,r.route,r.device,r.caller,r.instance,r.shared,r.ribbon,r.path,r.type,r.vertices,r.primitives,r.indexed,r.userMemory);
                WLOG_INFO("m2-draw-trace-gpu: frame=%u row=%u valid=%#x rt=%p rtTexture=%p backbuffer=%p size=%ux%u format=%u viewport=%u,%u,%u,%u tex0=%p tex1=%p tex2=%p vs=%p ps=%p colorWrite=%#lx zWrite=%lu zEnable=%lu",traceFrame,n,s.valid,reinterpret_cast<void*>(s.rt),reinterpret_cast<void*>(s.rtTexture),reinterpret_cast<void*>(s.backbuffer),s.desc.Width,s.desc.Height,unsigned(s.desc.Format),s.viewport.X,s.viewport.Y,s.viewport.Width,s.viewport.Height,reinterpret_cast<void*>(s.texture[0]),reinterpret_cast<void*>(s.texture[1]),reinterpret_cast<void*>(s.texture[2]),reinterpret_cast<void*>(s.vs),reinterpret_cast<void*>(s.ps),s.colorWrite,s.zWrite,s.zEnable);
            }
            WLOG_INFO("m2-draw-trace: end frame=%u rows=%u",traceFrame,traceCount);
        }
        ++traceFrame;traceCount=0;traceDropped=0;traceEngine=0;traceEngineRow=UINT_MAX;
        traceCapture=traceSchedule.Next(GetTickCount(),traceTargetSeen);
        traceTargetSeen=false;
    }
    void __cdecl TraceLost(void*,const void*) {
        if(!pl::TraceEnabled())return;
        WLOG_WARN("m2-draw-trace: device-lost frame=%u partialRows=%u; partial interval discarded",traceFrame,traceCount);
        traceCapture=false;traceCount=0;traceDropped=0;traceTargetSeen=false;
    }

    // 0 = waiting for the exact first body draw, 1 = one render thread owns the readback, 2 = done.
    // The render path is normally single-threaded, but an interlocked guard keeps this diagnostic
    // one-shot even if a future scene driver submits from more than one worker.
    volatile LONG g_orcFemaleD3DProbeState = 0;

    /**
     * @brief Detours the M2 batch draw, recording the drawing model so the per-draw event can name it.
     * @param ctx  draw context carrying the model field.
     * @param edx  unused register slot for the thiscall convention.
     */
    void __fastcall hkDrawBatch(void* ctx, void* edx)
    {
        void* prevModel = g_curModel;
        void* prevCtx   = g_curDrawCtx;
        g_curModel = static_cast<off::DrawBatchContext*>(ctx)->model;
        g_curDrawCtx = ctx;
        g_origDrawBatch(ctx, edx);
        g_curDrawCtx = prevCtx;
        g_curModel = prevModel;
    }

    /**
     * @brief Rejects the exact zero-stride state that makes the stock device draw divide by zero.
     *
     * This intentionally mirrors the stock outer gates and leaves user-memory draws untouched. It
     * does not repair or synthesize geometry: only the invalid draw is omitted, and later valid draws
     * continue through the original dispatcher normally.
     */
    void DeviceDrawChecked(void* device, void* edx, int* primitiveBatch, int indexed)
    {
        bool     invalidStride = false;
        void*    vertexBuffer  = nullptr;
        uint32_t byteSize      = 0;

        __try
        {
            const auto* base = static_cast<const uint8_t*>(device);
            const bool stockWouldDraw =
                *reinterpret_cast<const uint32_t*>(base + off::kDeviceDrawSceneActive) != 0 &&
                *reinterpret_cast<const uint32_t*>(base + off::kDeviceDrawSuppressed) == 0;
            const bool usesBoundVertexBuffer =
                *reinterpret_cast<const uint32_t*>(base + off::kDeviceDrawUserMemory) == 0;

            if (stockWouldDraw && usesBoundVertexBuffer)
            {
                vertexBuffer = *reinterpret_cast<void* const*>(
                    base + off::kDeviceCurrentVertexBuffer);
                if (vertexBuffer)
                {
                    const auto* vb = static_cast<const uint8_t*>(vertexBuffer);
                    const uint32_t stride = *reinterpret_cast<const uint32_t*>(
                        vb + off::kVertexBufferStride);
                    byteSize = *reinterpret_cast<const uint32_t*>(
                        vb + off::kVertexBufferByteSize);
                    invalidStride = stride == 0;
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            // Preserve native behavior for any state other than the confirmed zero-stride case.
            invalidStride = false;
        }

        if (invalidStride)
        {
            const LONG skipped = InterlockedIncrement(&g_zeroStrideDrawSkips);
            if (skipped <= 16 || (skipped & (skipped - 1)) == 0)
            {
                int primitive = -1;
                int count = -1;
                __try
                {
                    if (primitiveBatch)
                    {
                        primitive = primitiveBatch[0];
                        count = primitiveBatch[2];
                    }
                }
                __except (EXCEPTION_EXECUTE_HANDLER) {}

                WLOG_WARN("m2-draw: skipped zero-stride engine draw #%ld "
                          "(vb=%p bytes=%u batch=%p primitive=%d count=%d indexed=%d model=%p)",
                          skipped, vertexBuffer, byteSize, static_cast<void*>(primitiveBatch),
                          primitive, count, indexed, g_curModel);
            }
            return;
        }

        g_origDeviceDraw(device, edx, primitiveBatch, indexed);
    }
    void __fastcall hkDeviceDraw(void* device,void* edx,int* primitiveBatch,int indexed)
    {
        TraceEngineScope trace(device,primitiveBatch,indexed,_ReturnAddress());
        DeviceDrawChecked(device,edx,primitiveBatch,indexed);
    }

    /**
     * @brief Re-expands modern skin section index starts before the D3D draw.
     *
     * The client's M2 draw path truncates M2SkinSection::indexStart to 16 bits when passing StartIndex to
     * DrawIndexedPrimitive. Retail character and equipment skins use section.level as the high 16 bits of
     * the index window.
     * By the time DIP is called, drawCtx+0x90 points at the copied M2SkinSection for this batch, so the vtable
     * hook can restore the full 32-bit StartIndex without touching normal legacy sections.
     */
    unsigned ExpandM2StartIndex(unsigned startIndex) noexcept
    {
        if (!g_curDrawCtx) return startIndex;
        __try
        {
            const auto* ctx = static_cast<const off::DrawBatchContext*>(g_curDrawCtx);
            const auto* sec = static_cast<const wxl::structure::m2::M2SkinSection*>(ctx->section);
            if (!sec || sec->level == 0) return startIndex;
            if ((startIndex & 0xFFFFu) != sec->indexStart) return startIndex;
            const unsigned expanded = (static_cast<unsigned>(sec->level) << 16) | sec->indexStart;
            static unsigned logged = 0;
            if (logged < 16)
            {
                ++logged;
                WLOG_INFO("m2-draw: expanded M2 StartIndex %u -> %u (section=%u level=%u count=%u)",
                          startIndex, expanded,
                          static_cast<unsigned>(sec->skinSectionId),
                          static_cast<unsigned>(sec->level),
                          static_cast<unsigned>(sec->indexCount));
            }
            return expanded;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return startIndex;
        }
    }

    /**
     * @brief Reads back one exact female-Orc base-body draw without changing either GPU buffer.
     *
     * The probe is deliberately downstream of the model/skin rebuilds: GetStreamSource and
     * GetIndices name the resources D3D will actually consume.  A READONLY lock is attempted over
     * only this draw's byte windows.  Some WotLK buffers are declared WRITEONLY; in that case D3D9
     * normally rejects READONLY and a flags=0 fallback would make CPU reads undefined, so it is
     * explicitly not attempted.  The fallback is used only for a non-WRITEONLY descriptor.
     */
    void ProbeFemaleOrcD3DGeometry(void* rawDevice, int baseVertexIndex,
                                   unsigned minVertexIndex, unsigned numVertices,
                                   unsigned drawStartIndex, unsigned primitiveCount) noexcept
    {
        if (!rawDevice ||
            InterlockedCompareExchange(&g_orcFemaleD3DProbeState, 2, 2) == 2)
            return;

        IDirect3DVertexBuffer9* vb = nullptr;
        IDirect3DIndexBuffer9* ib = nullptr;
        void* vbBytes = nullptr;
        void* ibBytes = nullptr;
        bool vbLocked = false;
        bool ibLocked = false;
        bool faulted = false;
        void* shared = nullptr;
        const wxl::structure::m2::M2SkinSection* section = nullptr;
        const char* path = nullptr;

        __try
        {
            auto* const dc = static_cast<const off::DrawBatchContext*>(g_curDrawCtx);
            section = dc
                ? static_cast<const wxl::structure::m2::M2SkinSection*>(dc->section) : nullptr;
            shared = g_curModel
                ? *reinterpret_cast<void**>(static_cast<uint8_t*>(g_curModel) + m2off::kOffInstModel)
                : nullptr;
            path = shared && wxl::modern::assets::m2::IsNativeLoaded(shared)
                ? wxl::game::m2::M2Model(shared).GetPathStem() : nullptr;

            const bool exactPath = path &&
                (std::strcmp(path, "character\\orc\\female\\orcfemale_hd.m2") == 0 ||
                 std::strcmp(path, "character\\orc\\female\\orcfemale_hd") == 0 ||
                 std::strcmp(path, "character\\human\\male\\humanmale_hd.m2") == 0 ||
                 std::strcmp(path, "character\\human\\male\\humanmale_hd") == 0);
            if (!exactPath || !section || section->skinSectionId != 0)
                return;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return;
        }

        if (InterlockedCompareExchange(&g_orcFemaleD3DProbeState, 1, 0) != 0)
            return;

        __try
        {
            auto* const header = wxl::game::m2::M2Model(shared).GetHeader();
            auto* const skin = wxl::game::m2::M2Model(shared).GetSkin();
            auto* const device = static_cast<IDirect3DDevice9*>(rawDevice);
            const auto* const modelVertices = header && header->vertices.offset
                ? reinterpret_cast<const uint8_t*>(
                    static_cast<uintptr_t>(header->vertices.offset)) : nullptr;

            if (!header || !skin || !modelVertices || !header->vertices.count ||
                !skin->vertexLookup || !skin->indices || !skin->indexCount)
            {
                WLOG_WARN("orc-female-d3d: source unavailable header=%p skin=%p modelVertices=%p"
                          " lookup=%p skinBones=%p indices=%p modelVertexCount=%u"
                          " skinVertexCount=%u indexCount=%u path='%s'",
                          header, skin, modelVertices,
                          skin ? skin->vertexLookup : nullptr,
                          skin ? skin->bones : nullptr,
                          skin ? skin->indices : nullptr,
                          header ? header->vertices.count : 0,
                          skin ? skin->vertexCount : 0,
                          skin ? skin->indexCount : 0, path);
            }
            else
            {
                UINT streamOffset = 0;
                UINT streamStride = 0;
                D3DVERTEXBUFFER_DESC vbDesc{};
                D3DINDEXBUFFER_DESC ibDesc{};
                const HRESULT hrStream = device->GetStreamSource(
                    0, &vb, &streamOffset, &streamStride);
                const HRESULT hrIndices = device->GetIndices(&ib);
                const HRESULT hrVbDesc = SUCCEEDED(hrStream) && vb
                    ? vb->GetDesc(&vbDesc) : E_FAIL;
                const HRESULT hrIbDesc = SUCCEEDED(hrIndices) && ib
                    ? ib->GetDesc(&ibDesc) : E_FAIL;

                const uint32_t sectionStart =
                    (static_cast<uint32_t>(section->level) << 16) | section->indexStart;
                const uint64_t primitiveIndices64 = static_cast<uint64_t>(primitiveCount) * 3u;
                const uint32_t drawIndexCount = static_cast<uint32_t>(
                    primitiveIndices64 < section->indexCount
                        ? primitiveIndices64 : section->indexCount);

                WLOG_INFO("orc-female-d3d: begin device=%p vb=%p ib=%p streamHr=%#lx"
                          " indicesHr=%#lx offset=%u stride=%u sectionStart=%u drawStart=%u"
                          " indexCount=%u/%u bv=%d min=%u vertices=%u path='%s'",
                          device, vb, ib,
                          static_cast<unsigned long>(hrStream),
                          static_cast<unsigned long>(hrIndices),
                          streamOffset, streamStride, sectionStart, drawStartIndex,
                          drawIndexCount, static_cast<unsigned>(section->indexCount),
                          baseVertexIndex, minVertexIndex, numVertices, path);

                if (SUCCEEDED(hrVbDesc))
                    WLOG_INFO("orc-female-d3d: VB desc size=%u usage=%#lx pool=%u"
                              " format=%u type=%u fvf=%#lx source=%p lookup=%p bones=%p",
                              vbDesc.Size, static_cast<unsigned long>(vbDesc.Usage),
                              static_cast<unsigned>(vbDesc.Pool),
                              static_cast<unsigned>(vbDesc.Format),
                              static_cast<unsigned>(vbDesc.Type),
                              static_cast<unsigned long>(vbDesc.FVF),
                              modelVertices, skin->vertexLookup, skin->bones);
                else
                    WLOG_WARN("orc-female-d3d: VB descriptor unavailable hr=%#lx",
                              static_cast<unsigned long>(hrVbDesc));

                if (SUCCEEDED(hrIbDesc))
                    WLOG_INFO("orc-female-d3d: IB desc size=%u usage=%#lx pool=%u"
                              " format=%u type=%u source=%p",
                              ibDesc.Size, static_cast<unsigned long>(ibDesc.Usage),
                              static_cast<unsigned>(ibDesc.Pool),
                              static_cast<unsigned>(ibDesc.Format),
                              static_cast<unsigned>(ibDesc.Type), skin->indices);
                else
                    WLOG_WARN("orc-female-d3d: IB descriptor unavailable hr=%#lx",
                              static_cast<unsigned long>(hrIbDesc));

                // Read and compare the exact GPU index window against the section's canonical live
                // skin window.  The GPU offset deliberately uses the DIP argument; the expected
                // pointer deliberately uses the full section start so a truncation is observable.
                HRESULT hrIbLock = E_FAIL;
                DWORD ibLockFlags = D3DLOCK_READONLY;
                uint32_t ibMismatch = 0;
                uint32_t ibOutsideDraw = 0;
                uint32_t ibSamples = 0;
                const uint32_t ibElementSize = SUCCEEDED(hrIbDesc)
                    ? (ibDesc.Format == D3DFMT_INDEX16 ? 2u
                       : (ibDesc.Format == D3DFMT_INDEX32 ? 4u : 0u)) : 0u;
                const uint64_t gpuIbEnd =
                    (static_cast<uint64_t>(drawStartIndex) + drawIndexCount) * ibElementSize;
                const bool ibSourceFits = sectionStart <= skin->indexCount &&
                    drawIndexCount <= skin->indexCount - sectionStart;
                const bool ibGpuFits = ibElementSize && gpuIbEnd <= ibDesc.Size;
                const bool ibWriteOnly = SUCCEEDED(hrIbDesc) &&
                    (ibDesc.Usage & D3DUSAGE_WRITEONLY) != 0;

                if (ib && SUCCEEDED(hrIbDesc) && drawIndexCount && ibSourceFits && ibGpuFits)
                {
                    hrIbLock = ib->Lock(drawStartIndex * ibElementSize,
                                        drawIndexCount * ibElementSize,
                                        &ibBytes, D3DLOCK_READONLY);
                    ibLocked = SUCCEEDED(hrIbLock);
                    if (hrIbLock == D3DERR_INVALIDCALL && !ibWriteOnly)
                    {
                        ibLockFlags = 0;
                        hrIbLock = ib->Lock(drawStartIndex * ibElementSize,
                                            drawIndexCount * ibElementSize, &ibBytes, 0);
                        ibLocked = SUCCEEDED(hrIbLock);
                    }

                    // Even an unexpectedly successful READONLY lock on a WRITEONLY resource does
                    // not make reading it defined by D3D9; unlock it without touching the bytes.
                    if (ibLocked && !ibWriteOnly)
                    {
                        for (uint32_t i = 0; i < drawIndexCount; ++i)
                        {
                            const uint32_t gpu = ibElementSize == 2
                                ? static_cast<const uint16_t*>(ibBytes)[i]
                                : static_cast<const uint32_t*>(ibBytes)[i];
                            const uint32_t expected = skin->indices[sectionStart + i];
                            const bool mismatch = gpu != expected;
                            const uint64_t drawVertexEnd =
                                static_cast<uint64_t>(minVertexIndex) + numVertices;
                            const bool outside = gpu < minVertexIndex || gpu >= drawVertexEnd;
                            if (mismatch) ++ibMismatch;
                            if (outside) ++ibOutsideDraw;
                            if ((mismatch || outside) && ibSamples < 4)
                            {
                                ++ibSamples;
                                WLOG_INFO("orc-female-d3d: IB sample i=%u gpu=%u expected=%u"
                                          " mismatch=%u outsideDraw=%u",
                                          i, gpu, expected, mismatch ? 1u : 0u,
                                          outside ? 1u : 0u);
                            }
                        }
                    }
                }

                WLOG_INFO("orc-female-d3d: IB read hr=%#lx flags=%#lx writeOnly=%u"
                          " sourceFits=%u gpuFits=%u formatBytes=%u startMatch=%u"
                          " compared=%u mismatches=%u outsideDraw=%u%s",
                          static_cast<unsigned long>(hrIbLock),
                          static_cast<unsigned long>(ibLockFlags), ibWriteOnly ? 1u : 0u,
                          ibSourceFits ? 1u : 0u, ibGpuFits ? 1u : 0u, ibElementSize,
                          sectionStart == drawStartIndex ? 1u : 0u,
                          ibLocked && !ibWriteOnly ? drawIndexCount : 0u,
                          ibMismatch, ibOutsideDraw,
                          ibWriteOnly ? " (flags=0 fallback skipped: CPU read is undefined)" : "");

                // Stream zero contains one vertex per live skin lookup entry.  Compare the complete
                // draw range's positions and all raw fields except the four register-slot bytes;
                // those slots are checked separately against skin->bones because geometry splitting
                // can legitimately give two live lookup entries for one source model vertex.
                HRESULT hrVbLock = E_FAIL;
                DWORD vbLockFlags = D3DLOCK_READONLY;
                uint32_t vbCompared = 0;
                uint32_t vbInvalidLookup = 0;
                uint32_t vbPositionMismatch = 0;
                uint32_t vbRecordSansBoneMismatch = 0;
                uint32_t vbRawRecordMismatch = 0;
                uint32_t vbSlotMismatch = 0;
                uint32_t vbSamples = 0;
                const int64_t physicalVertexStart =
                    static_cast<int64_t>(baseVertexIndex) + minVertexIndex;
                const uint64_t logicalVertexEnd =
                    static_cast<uint64_t>(minVertexIndex) + numVertices;
                const uint64_t vbByteOffset64 = physicalVertexStart >= 0
                    ? static_cast<uint64_t>(streamOffset) +
                        static_cast<uint64_t>(physicalVertexStart) * streamStride : ~uint64_t(0);
                const uint64_t vbByteSize64 = static_cast<uint64_t>(numVertices) * streamStride;
                const bool vbSourceFits = logicalVertexEnd <= skin->vertexCount;
                const bool vbGpuFits = SUCCEEDED(hrVbDesc) && physicalVertexStart >= 0 &&
                    streamStride >= 12 && numVertices <= 0x10000u &&
                    vbByteOffset64 <= vbDesc.Size && vbByteSize64 <= vbDesc.Size - vbByteOffset64;
                const bool vbWriteOnly = SUCCEEDED(hrVbDesc) &&
                    (vbDesc.Usage & D3DUSAGE_WRITEONLY) != 0;

                if (vb && SUCCEEDED(hrVbDesc) && numVertices && vbSourceFits && vbGpuFits)
                {
                    hrVbLock = vb->Lock(static_cast<UINT>(vbByteOffset64),
                                        static_cast<UINT>(vbByteSize64),
                                        &vbBytes, D3DLOCK_READONLY);
                    vbLocked = SUCCEEDED(hrVbLock);
                    if (hrVbLock == D3DERR_INVALIDCALL && !vbWriteOnly)
                    {
                        vbLockFlags = 0;
                        hrVbLock = vb->Lock(static_cast<UINT>(vbByteOffset64),
                                            static_cast<UINT>(vbByteSize64), &vbBytes, 0);
                        vbLocked = SUCCEEDED(hrVbLock);
                    }

                    if (vbLocked && !vbWriteOnly)
                    {
                        for (uint32_t i = 0; i < numVertices; ++i)
                        {
                            const uint32_t logical = minVertexIndex + i;
                            const uint32_t source = skin->vertexLookup[logical];
                            const auto* const gpu = static_cast<const uint8_t*>(vbBytes) +
                                static_cast<size_t>(i) * streamStride;
                            if (source >= header->vertices.count)
                            {
                                ++vbInvalidLookup;
                                continue;
                            }

                            const auto* const expected = modelVertices +
                                static_cast<size_t>(source) * 0x30u;
                            const bool posMismatch = std::memcmp(gpu, expected, 12) != 0;
                            bool sansBoneMismatch = false;
                            bool rawMismatch = false;
                            bool slotMismatch = false;
                            if (streamStride >= 0x30u)
                            {
                                rawMismatch = std::memcmp(gpu, expected, 0x30u) != 0;
                                sansBoneMismatch = std::memcmp(gpu, expected, 0x10u) != 0 ||
                                    std::memcmp(gpu + 0x14u, expected + 0x14u, 0x1Cu) != 0;
                                slotMismatch = skin->bones &&
                                    std::memcmp(gpu + 0x10u,
                                                skin->bones + static_cast<size_t>(logical) * 4u,
                                                4u) != 0;
                            }

                            ++vbCompared;
                            if (posMismatch) ++vbPositionMismatch;
                            if (sansBoneMismatch) ++vbRecordSansBoneMismatch;
                            if (rawMismatch) ++vbRawRecordMismatch;
                            if (slotMismatch) ++vbSlotMismatch;
                            if ((posMismatch || sansBoneMismatch || slotMismatch) && vbSamples < 4)
                            {
                                ++vbSamples;
                                float gpuPos[3]{};
                                float expectedPos[3]{};
                                std::memcpy(gpuPos, gpu, sizeof(gpuPos));
                                std::memcpy(expectedPos, expected, sizeof(expectedPos));
                                const uint8_t* const liveSlots = skin->bones
                                    ? skin->bones + static_cast<size_t>(logical) * 4u : nullptr;
                                WLOG_INFO("orc-female-d3d: VB sample logical=%u physical=%lld"
                                          " source=%u gpuPos=%.6g,%.6g,%.6g"
                                          " expectedPos=%.6g,%.6g,%.6g"
                                          " gpuSlots=%u,%u,%u,%u liveSlots=%u,%u,%u,%u"
                                          " posMismatch=%u sansBoneMismatch=%u slotMismatch=%u",
                                          logical, static_cast<long long>(physicalVertexStart + i),
                                          source, gpuPos[0], gpuPos[1], gpuPos[2],
                                          expectedPos[0], expectedPos[1], expectedPos[2],
                                          streamStride >= 0x14u ? gpu[0x10] : 0,
                                          streamStride >= 0x14u ? gpu[0x11] : 0,
                                          streamStride >= 0x14u ? gpu[0x12] : 0,
                                          streamStride >= 0x14u ? gpu[0x13] : 0,
                                          liveSlots ? liveSlots[0] : 0,
                                          liveSlots ? liveSlots[1] : 0,
                                          liveSlots ? liveSlots[2] : 0,
                                          liveSlots ? liveSlots[3] : 0,
                                          posMismatch ? 1u : 0u,
                                          sansBoneMismatch ? 1u : 0u,
                                          slotMismatch ? 1u : 0u);
                            }
                        }
                    }
                }

                WLOG_INFO("orc-female-d3d: VB read hr=%#lx flags=%#lx writeOnly=%u"
                          " sourceFits=%u gpuFits=%u compared=%u invalidLookup=%u"
                          " positionMismatch=%u sansBoneMismatch=%u rawMismatch=%u"
                          " slotMismatch=%u%s",
                          static_cast<unsigned long>(hrVbLock),
                          static_cast<unsigned long>(vbLockFlags), vbWriteOnly ? 1u : 0u,
                          vbSourceFits ? 1u : 0u, vbGpuFits ? 1u : 0u,
                          vbCompared, vbInvalidLookup, vbPositionMismatch,
                          vbRecordSansBoneMismatch, vbRawRecordMismatch, vbSlotMismatch,
                          vbWriteOnly ? " (flags=0 fallback skipped: CPU read is undefined)" : "");
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            faulted = true;
        }

        // Do not rely on C++ unwinding across SEH.  Every COM/lock cleanup gets its own guard so an
        // unusual driver failure cannot strand the once-state or turn a diagnostic into a crash.
        if (ibLocked && ib)
            __try { ib->Unlock(); } __except (EXCEPTION_EXECUTE_HANDLER) { faulted = true; }
        if (vbLocked && vb)
            __try { vb->Unlock(); } __except (EXCEPTION_EXECUTE_HANDLER) { faulted = true; }
        if (ib)
            __try { ib->Release(); } __except (EXCEPTION_EXECUTE_HANDLER) { faulted = true; }
        if (vb)
            __try { vb->Release(); } __except (EXCEPTION_EXECUTE_HANDLER) { faulted = true; }

        if (faulted)
            WLOG_WARN("orc-female-d3d: guarded probe/cleanup fault; draw left unmodified");
        InterlockedExchange(&g_orcFemaleD3DProbeState, 2);
    }

    /**
     * @brief Folds a three-layer ribbon's bound textures into one fixed-function pass.
     *
     * Combines tex0*tex1*tex2*color*4 (MODULATE, MODULATE, MODULATE4X). Stage state is saved and
     * restored so the next draw is unaffected; the additive frame blend the emitter set stays in place.
     * @param dev  D3D9 device.
     * @param pt   primitive type.
     * @param bv   base vertex index.
     * @param mi   minimum vertex index.
     * @param nv   vertex count.
     * @param si   start index.
     * @param pc   primitive count.
     * @return the DrawIndexedPrimitive result.
     */
    long DrawRibbonMultiTexture(IDirect3DDevice9* dev, int pt, int bv, unsigned mi, unsigned nv, unsigned si, unsigned pc)
    {
        DWORD s[4][4];
        for (DWORD st = 0; st < 4; ++st)
        {
            dev->GetTextureStageState(st, D3DTSS_COLOROP,   &s[st][0]);
            dev->GetTextureStageState(st, D3DTSS_COLORARG1, &s[st][1]);
            dev->GetTextureStageState(st, D3DTSS_COLORARG2, &s[st][2]);
            dev->GetTextureStageState(st, D3DTSS_ALPHAOP,   &s[st][3]);
        }

        const D3DTEXTUREOP op[3] = { D3DTOP_MODULATE, D3DTOP_MODULATE, D3DTOP_MODULATE4X };
        for (DWORD st = 0; st < 3; ++st)
        {
            dev->SetTextureStageState(st, D3DTSS_COLOROP,   op[st]);
            dev->SetTextureStageState(st, D3DTSS_COLORARG1, D3DTA_TEXTURE);
            dev->SetTextureStageState(st, D3DTSS_COLORARG2, st == 0 ? D3DTA_DIFFUSE : D3DTA_CURRENT);
            dev->SetTextureStageState(st, D3DTSS_ALPHAOP,   op[st]);
            dev->SetTextureStageState(st, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
            dev->SetTextureStageState(st, D3DTSS_ALPHAARG2, st == 0 ? D3DTA_DIFFUSE : D3DTA_CURRENT);
        }
        dev->SetTextureStageState(3, D3DTSS_COLOROP, D3DTOP_DISABLE);

        long r = DrawNativeWithShadows(dev, pt, bv, mi, nv, si, pc);

        for (DWORD st = 0; st < 4; ++st)
        {
            dev->SetTextureStageState(st, D3DTSS_COLOROP,   s[st][0]);
            dev->SetTextureStageState(st, D3DTSS_COLORARG1, s[st][1]);
            dev->SetTextureStageState(st, D3DTSS_COLORARG2, s[st][2]);
            dev->SetTextureStageState(st, D3DTSS_ALPHAOP,   s[st][3]);
        }
        return r;
    }

    /** @brief Returns the graphics-device object carrying the engine sampler-bind path (distinct from the D3D9 device). */
    void* GxDeviceObject() { return *reinterpret_cast<void**>(off::kGxDevicePtr); }

    /**
     * @brief Binds ribbon layers 1 and 2 to samplers s1/s2 through the engine for the single pass.
     *
     * Called only with layerCount >= 3, so layers [1] and [2] are in range.
     * @param gxDev    graphics-device object.
     * @param emitter  ribbon emitter holding the texture handle array.
     * @return true when both layers resolved and bound.
     */
    bool BindRibbonExtraSamplers(void* gxDev, const uint8_t* emitter)
    {
        const void* const* arr = reinterpret_cast<const m2off::RibbonEmitter*>(emitter)->texHandles;
        if (!arr) return false;
        void* h1 = const_cast<void*>(arr[1]);
        void* h2 = const_cast<void*>(arr[2]);
        if (!h1 || !h2) return false;

        auto resolve = reinterpret_cast<m2off::M2_TexResolveFn>(m2off::kTexResolve);
        auto bind    = reinterpret_cast<m2off::M2_SamplerBindFn>(m2off::kSamplerBind);
        void* t1 = resolve(h1, 0, 0);
        void* t2 = resolve(h2, 0, 0);
        if (!t1 || !t2) return false;
        bind(gxDev, nullptr, m2off::kSamplerSelS1, t1);
        bind(gxDev, nullptr, m2off::kSamplerSelS2, t2);
        return true;
    }

    /**
     * @brief Detours the ribbon emitter draw, emitting OnRibbonDraw and optionally folding layers.
     *
     * When a subscriber opts a three-or-more-layer ribbon into the multi-texture combine, pre-binds
     * s1/s2, flags the draw so the DIP override folds the layers, and clamps the layer count to 1 so
     * the native draw runs exactly one pass. Otherwise the draw runs untouched.
     * @param self        ribbon emitter.
     * @param edx         unused register slot for the thiscall convention.
     * @param stateBlock  native render state block.
     * @return the native ribbon-draw result.
     */
    int __fastcall hkRibbonDraw(void* self, void* edx, void* stateBlock)
    {
        void* previousTraceRibbon=traceRibbon;traceRibbon=self;
        void* previousRibbonEmitter=wxl::modern::ribbon::SetEmitter(self);
        g_ribbonModern = false;

        uint8_t*  emitter       = static_cast<uint8_t*>(self);
        bool      bridged       = false;
        uint32_t  savedLayers   = 0;
        uint32_t* layerCountPtr = nullptr;

        if (emitter && !wxl::modern::ribbon::Owns(emitter))
        {
            __try
            {
                layerCountPtr = &reinterpret_cast<m2off::RibbonEmitter*>(emitter)->layerCount;
                uint32_t layers = *layerCountPtr;

                bool useMulti = false;
                ev::RibbonDrawArgs a{ emitter, layers, &useMulti };
                wxl_modern_m2::g_api->Emit(uint32_t(ev::Event::OnRibbonDraw), &a);

                if (useMulti && layers >= 3)
                {
                    void* gxDev = GxDeviceObject();
                    if (gxDev && BindRibbonExtraSamplers(gxDev, emitter))
                    {
                        g_ribbonModern = true;
                        savedLayers    = layers;
                        *layerCountPtr = 1;
                        bridged        = true;
                    }
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER) { bridged = false; g_ribbonModern = false; }
        }

        int r = g_origRibbonDraw(self, edx, stateBlock);

        if (bridged && layerCountPtr)
            __try { *layerCountPtr = savedLayers; } __except (EXCEPTION_EXECUTE_HANDLER) {}
        g_ribbonModern = false;
        traceRibbon=previousTraceRibbon;
        wxl::modern::ribbon::SetEmitter(previousRibbonEmitter);
        return r;
    }

    /**
     * @brief DrawIndexedPrimitive detour: serves an armed one-shot interceptor, folds multi-texture
     *        ribbons, and emits OnM2BatchDraw.
     *
     * An armed interceptor consumes exactly this draw (cleared first, so its own re-issue through the
     * hooked vtable runs native). Otherwise the native draw runs (with the 32-bit M2 start index
     * restored) and the M2 batch is published with its draw parameters. Guarded so a subscriber
     * re-issue does not recurse.
     */
    long __stdcall hkDIP(void* dev, int pt, int bv, unsigned mi, unsigned nv, unsigned si, unsigned pc)
    {
        wxl::modern::shadowcapture::Observe(dev,(g_oneShot ? 1u : 0u) | (g_ribbonModern ? 2u : 0u) |
            (g_curModel ? 4u : 0u) | (wxl::runtime::m2shadow::HasShadowContext() ? 8u : 0u),nv,pc);
        si = wxl::runtime::m2shadow::PrepareDIP(dev,pt,si,pc);
        wxl::runtime::m2shadow::BeforeDIP(dev, si, pc);
        TraceDraw(dev,_ReturnAddress(),1,pt,nv,pc,1);
        if(traceCapture&&traceEngineRow!=UINT_MAX)++traceRows[traceEngineRow].dips;
        wxl::modern::particlelayers::ObserveDIP(g_oneShot!=nullptr,g_ribbonModern);
        if (WXL_M2Draw_InterceptFn intercept = g_oneShot)
        {
            g_oneShot = nullptr;
            return intercept(dev, pt, bv, mi, nv, si, pc, DrawNativeWithShadows);
        }
        if (g_ribbonModern)
            return DrawRibbonMultiTexture(static_cast<IDirect3DDevice9*>(dev), pt, bv, mi, nv, si, pc);

        if(wxl::modern::ribbon::Active())
            return wxl::modern::ribbon::Draw(dev,pt,bv,mi,nv,si,pc,DrawNativeWithShadows);

        wxl::modern::particlediag::BeforeDIP(dev, si, pc);
        if (wxl::modern::particlelayers::Active())
            return wxl::modern::particlelayers::DrawDIP(dev, pt, bv, mi, nv, si, pc, DrawNativeWithShadows);
        const unsigned drawStartIndex = wxl::runtime::m2shadow::HasShadowContext()
            ? si : ExpandM2StartIndex(si);
        ProbeFemaleOrcD3DGeometry(dev, bv, mi, nv, drawStartIndex, pc);
        wxl::modern::materialdiag::BeforeDraw(dev, g_curDrawCtx, g_curModel, drawStartIndex, pc);
        long r = wxl::modern::materialblend::Draw(dev, g_curDrawCtx, g_curModel,
            pt, bv, mi, nv, drawStartIndex, pc, DrawNativeWithShadows);

        // One bounded proof that the Retail Orc-female base model reaches D3D after visibility,
        // optimized-list construction, material setup and bone-palette upload. g_curModel is the
        // instance stored by the draw context; its +0x2C field is the shared model carrying the path.
        __try
        {
            void* const shared = g_curModel
                ? *reinterpret_cast<void**>(static_cast<uint8_t*>(g_curModel) + m2off::kOffInstModel)
                : nullptr;
            const char* const path = shared && wxl::modern::assets::m2::IsNativeLoaded(shared)
                ? wxl::game::m2::M2Model(shared).GetPathStem() : nullptr;
            if (path && std::strcmp(path, "character\\orc\\female\\orcfemale_hd.m2") == 0)
            {
                static uint32_t drawReports = 0;
                if (drawReports < 32)
                {
                    ++drawReports;
                    const auto* const dc = static_cast<const off::DrawBatchContext*>(g_curDrawCtx);
                    const auto* const section = dc
                        ? static_cast<const wxl::structure::m2::M2SkinSection*>(dc->section) : nullptr;
                    const float alpha = dc && dc->element
                        ? *reinterpret_cast<const float*>(
                            static_cast<const uint8_t*>(dc->element) + m2off::kOffElementAlpha)
                        : -1.0f;
                    WLOG_INFO("char-draw: Orc female section=%u level=%u alpha=%.6f"
                              " bv=%d min=%u verts=%u start=%u prims=%u hr=%#lx",
                              section ? static_cast<unsigned>(section->skinSectionId) : 0xFFFFFFFFu,
                              section ? static_cast<unsigned>(section->level) : 0xFFFFFFFFu,
                              alpha, bv, mi, nv, drawStartIndex, pc, r);
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}

        if (g_curModel && !g_inM2Emit)
        {
            g_inM2Emit = true;
            ev::M2BatchDrawArgs a{ dev, g_curModel, pt, bv, mi, nv, drawStartIndex, pc };
            wxl_modern_m2::g_api->Emit(uint32_t(ev::Event::OnM2BatchDraw), &a);
            g_inM2Emit = false;
        }
        return r;
    }

    void __cdecl ApiSetOneShotIntercept(WXL_M2Draw_InterceptFn fn)
    {
        g_oneShot = fn;
    }

    WXL_M2DrawApi g_m2DrawApi = {
        sizeof(WXL_M2DrawApi), WXL_M2DRAW_API_VERSION, &ApiSetOneShotIntercept,
    };

    /**
     * @brief Installs the DIP swap on the live device. Each device instance may carry its own vtable, so
     *        on a device recreate the swap is gone; this re-applies it on the current device. D3D9 can
     *        also restore the original vtable in-place during Reset, so the slot is authoritative; the
     *        device pointer alone is not proof that the hook survived. Comparing the slot still protects
     *        shared vtables from capturing hkDIP as its own original.
     */
    void EnsureDIPHook(IDirect3DDevice9* dev)
    {
        if (!dev) return;
        void** vtbl = *reinterpret_cast<void***>(dev);
        if (vtbl[off::vt::kDrawIndexedPrimitive] != reinterpret_cast<void*>(&hkDIP))
        {
            wxl::mem::SwapPointer(&vtbl[off::vt::kDrawIndexedPrimitive], reinterpret_cast<void*>(&hkDIP),
                                  reinterpret_cast<void**>(&g_origDIP));
            WLOG_INFO("m2-draw: DrawIndexedPrimitive hook installed (dev=%p)", static_cast<void*>(dev));
        }
    }

    /// Re-applies the DIP swap on the same per-frame cadence core's own Render.cpp uses for its three
    /// slots (OnWorldRenderEnd fires from hkWorldFinalize, right after core's own EnsureDeviceHooks) --
    /// so a device recreate is covered without wxl-modern-m2 needing its own world-boundary hook.
    void __cdecl OnWorldRenderEnd(void* /*user*/, const void* argsRaw)
    {
        const auto* a = static_cast<const ev::WorldRenderEndArgs*>(argsRaw);
        if (a && a->device)
            EnsureDIPHook(static_cast<IDirect3DDevice9*>(a->device));
    }

    /// Rendering can stop before OnWorldRenderEnd when D3D loses its context. The core's OnUpdate
    /// cadence remains alive and reconciles its own slots there; do the same for DIP so an in-place
    /// vtable restore cannot permanently drop modern M2 draw handling after recovery.
    void __cdecl OnUpdate(void* /*user*/, const void* /*argsRaw*/)
    {
        if (void* dev = gx::RawDevice())
            EnsureDIPHook(static_cast<IDirect3DDevice9*>(dev));
    }
}

namespace wxl_modern_m2
{
    bool InstallM2Draw()
    {
        wxl::modern::shadowcapture::Install();
        wxl::modern::terrainshadow::Install();
        wxl::modern::shaderobjects::Initialize();
        wxl::modern::materialdiag::Initialize();
        wxl::modern::materialblend::Initialize();
        wxl::modern::particlediag::Initialize();
        wxl::modern::ribbon::Initialize();
        if(pl::TraceEnabled()) {
            g_api->Subscribe(uint32_t(ev::Event::OnFrame),&TraceFrame,nullptr);
            g_api->Subscribe(uint32_t(ev::Event::OnDeviceLost),&TraceLost,nullptr);
            WLOG_WARN("m2-draw-trace: enabled; read-only eight Present intervals after target activity, 2048 rows per interval; kind 0=engine preflush, 1=DIP preoverride");
        }
        if (void* dev = gx::RawDevice())
            EnsureDIPHook(static_cast<IDirect3DDevice9*>(dev));
        else
            WLOG_WARN("m2-draw: device not up, DIP hook deferred to first world-render-end");

        g_api->Subscribe(uint32_t(ev::Event::OnWorldRenderEnd), &OnWorldRenderEnd, nullptr);
        g_api->Subscribe(uint32_t(ev::Event::OnUpdate), &OnUpdate, nullptr);

        HookAttachByName("M2.DrawBatch", &hkDrawBatch, &g_origDrawBatch);
        HookAttachByName("M2.RibbonDraw", &hkRibbonDraw, &g_origRibbonDraw);
        HookAttachByName("Gx.DeviceDraw", &hkDeviceDraw, &g_origDeviceDraw);

        g_api->PublishInterface("wxl.m2draw", WXL_M2DRAW_API_VERSION, &g_m2DrawApi);

        WLOG_INFO("m2-draw: batch/ribbon/device-draw detours installed, wxl.m2draw published");
        return true;
    }
}
