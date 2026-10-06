// M2 bone compatibility: post-fill bone-palette event, and the shadow-batch and main-draw doodad-batch
// detours the client carries exactly one owner of each for.
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

#include "../ExtensionApi.hpp"
#include "ShadowSpace.hpp"
#include "ShadowBatchLimit.hpp"
#include "LampLightPolicy.hpp"
#include "../compat/BoneBudget.hpp"
#include "../compat/ModernM2.hpp"

#include "engine/events/Event.hpp"
#include "engine/assets/shared/models/m2/M2Format.hpp"
#include "game/M2.hpp"

#include "offsets/engine/Gx.hpp"
#include "offsets/game/M2.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace
{
    namespace ev    = wxl::events;
    namespace m2    = wxl::offsets::game::m2;
    namespace gxoff = wxl::offsets::engine::gx;
    namespace bones = wxl::modern::assets::common::bones;

    m2::M2_BuildBonePaletteFn     g_origBuildBonePalette     = nullptr;
    m2::M2_RenderBatchShadowMapFn g_origRenderBatchShadowMap = nullptr;
    using SharedAllocInstancesFn = uint32_t (__fastcall*)(void*, void*, uint32_t);
    SharedAllocInstancesFn g_origSharedAllocInstances = nullptr;
    uint32_t g_shadowBatchMode = 1; // 0 historical splitter, 1 native capacity fix, 2 diagnostic hide
    bool g_singleShadowTest = false;
    std::atomic<uint32_t> g_singleShadowLogs{0};
    std::atomic<uint32_t> g_shadowBatchLogs{0};
    std::atomic<uint32_t> g_shadowGrantLogs{0};

    uint32_t __fastcall hkSharedAllocInstances(void* model, void* edx, uint32_t requested)
    {
        namespace sb = wxl_modern_m2::shadowbatch;
        const uint32_t available = g_origSharedAllocInstances(model, edx, sb::Capacity(model, requested));
        const uint32_t granted = sb::Capacity(model, available);
        // The allocator rounds up and may return a cached capacity: limiting its request alone
        // does not protect the bone constants. Never claim more storage than it actually returned.
        if (granted != available && g_shadowGrantLogs.fetch_add(1) < 24)
            WLOG_INFO("shadow-batch-v1: capacity shared=%p requested=%u available=%u granted=%u",
                      model, requested, available, granted);
        return granted;
    }

    // Separate from the SEH probe below so C++ scope unwinding stays well defined under /EHsc.
    void DrawShadowWithCapacity(void* model, uint32_t capacity, void* instance,
        uint32_t batchMode, void* skinBatch, void* drawList, uint32_t drawIndex,
        void* skinSection, void* previousSection)
    {
        wxl_modern_m2::shadowbatch::Scope scope(model, capacity);
        wxl::runtime::m2shadow::TraceScope trace(instance, skinSection, drawList, drawIndex);
        g_origRenderBatchShadowMap(instance, nullptr, batchMode, skinBatch, drawList,
                                   drawIndex, skinSection, previousSection);
    }

    // 0 = not reported, 1 = one animation thread owns the probe, 2 = reported.  The palette build
    // can run from the threaded scene driver, so a plain function-local bool is not a sufficient
    // once guard even for a diagnostic.
    volatile LONG g_orcFemaleMatrixProbeState = 0;

    using DrawBatchDoodadFn = void (__fastcall*)(void* ctx, void* edx, void* elements, void* indices);
    DrawBatchDoodadFn g_origDrawBatchDoodad = nullptr;

    /**
     * @brief Captures one exact female-Orc root palette after its last subscriber has written it.
     *
     * This does not lock or read back the D3D vertex buffer.  kSharedSetVertices' verified hardware
     * path takes the four GPU slot bytes from skin->bones, so this records that authoritative source,
     * its section-window lookup, and the instance matrix the draw will upload for the resulting global
     * bone.  All reads are bounded and SEH-contained because this runs in the animation hot path.
     */
    void ProbeFemaleOrcBonePalette(void* instance) noexcept
    {
        if (!instance || InterlockedCompareExchange(&g_orcFemaleMatrixProbeState, 2, 2) == 2)
            return;

        __try
        {
            auto* const inst = static_cast<uint8_t*>(instance);
            void* const shared = *reinterpret_cast<void**>(inst + m2::kOffInstModel);
            if (!shared) return;

            const char* const path = reinterpret_cast<const char*>(
                static_cast<const uint8_t*>(shared) + m2::kOffModelPathStem);
            const bool orcFemale = path &&
                (std::strcmp(path, "character\\orc\\female\\orcfemale_hd.m2") == 0 ||
                 std::strcmp(path, "character\\orc\\female\\orcfemale_hd") == 0);
            const bool humanMale = path &&
                (std::strcmp(path, "character\\human\\male\\humanmale_hd.m2") == 0 ||
                 std::strcmp(path, "character\\human\\male\\humanmale_hd") == 0);
            if (!orcFemale && !humanMale)
                return;

            if (InterlockedCompareExchange(&g_orcFemaleMatrixProbeState, 1, 0) != 0)
                return;

            auto* const header = *reinterpret_cast<wxl::structure::m2::M2Header**>(
                static_cast<uint8_t*>(shared) + m2::kOffModelHeader);
            auto* const skin = *reinterpret_cast<wxl::game::m2::M2SkinProfile**>(
                static_cast<uint8_t*>(shared) + m2::kOffModelSkin);
            float* const palette = *reinterpret_cast<float**>(inst + m2::kOffInstBonePalette);

            // Female Orc is 223 bones.  The generous ceiling keeps this diagnostic bounded while
            // making a wildly corrupt header a logged fact instead of an equally wild stack access.
            constexpr uint32_t kProbeBoneCeiling = 1024;
            constexpr uint32_t kSampleCount = 8;
            if (!header || !skin || !palette || !skin->submeshes || !skin->submeshCount ||
                !skin->vertexLookup || !skin->bones || !header->vertices.offset ||
                !header->boneCombos.offset || !header->bones.count ||
                header->bones.count > kProbeBoneCeiling)
            {
                WLOG_WARN("orc-female-matrices: unavailable instance=%p shared=%p header=%p skin=%p"
                          " palette=%p sections=%u vertices=%p skinBones=%p modelVertices=%p"
                          " combos=%p modelBones=%u path='%s'",
                          instance, shared, header, skin, palette, skin ? skin->submeshCount : 0,
                          skin ? skin->vertexLookup : nullptr, skin ? skin->bones : nullptr,
                          header ? reinterpret_cast<void*>(static_cast<uintptr_t>(header->vertices.offset)) : nullptr,
                          header ? reinterpret_cast<void*>(static_cast<uintptr_t>(header->boneCombos.offset)) : nullptr,
                          header ? header->bones.count : 0, path);
                InterlockedExchange(&g_orcFemaleMatrixProbeState, 2);
                return;
            }

            const uint16_t baseSectionId = skin->submeshes[0].skinSectionId;
            uint32_t sectionParts = 0;
            uint32_t sectionVertices = 0;
            while (sectionParts < skin->submeshCount &&
                   skin->submeshes[sectionParts].skinSectionId == baseSectionId)
            {
                const auto& part = skin->submeshes[sectionParts];
                const uint32_t partEnd = static_cast<uint32_t>(part.vertexStart) + part.vertexCount;
                if (partEnd > skin->vertexCount || part.boneCount > kProbeBoneCeiling)
                {
                    WLOG_WARN("orc-female-matrices: invalid base part=%u range=%u+%u/%u"
                              " bones=%u combo=%u path='%s'", sectionParts, part.vertexStart,
                              part.vertexCount, skin->vertexCount, part.boneCount,
                              part.boneComboIndex, path);
                    InterlockedExchange(&g_orcFemaleMatrixProbeState, 2);
                    return;
                }
                sectionVertices += part.vertexCount;
                ++sectionParts;
            }

            const auto* const vertices = reinterpret_cast<const uint8_t*>(
                static_cast<uintptr_t>(header->vertices.offset));
            const auto* const combos = reinterpret_cast<const uint16_t*>(
                static_cast<uintptr_t>(header->boneCombos.offset));

            bool seen[kProbeBoneCeiling]{};
            uint16_t sampleGlobal[kSampleCount]{};
            uint32_t sampleSkinVertex[kSampleCount]{};
            uint32_t sampleSourceVertex[kSampleCount]{};
            uint8_t sampleInfluence[kSampleCount]{};
            uint8_t sampleWeight[kSampleCount]{};
            uint8_t sampleSkinSlot[kSampleCount]{};
            uint8_t sampleRecordSlot[kSampleCount]{};
            uint16_t sampleSection[kSampleCount]{};
            uint32_t samples = 0;
            uint32_t positiveInfluences = 0;
            uint32_t recordSlotMismatches = 0;
            uint32_t mappingOutOfRange = 0;
            uint32_t uniqueBones = 0;
            uint32_t nonFiniteMatrices = 0;
            uint32_t zeroMatrices = 0;
            float maxAbsElement = 0.0f;

            for (uint32_t si = 0; si < sectionParts; ++si)
            {
                const auto& section = skin->submeshes[si];
                const uint32_t sectionEnd =
                    static_cast<uint32_t>(section.vertexStart) + section.vertexCount;
                for (uint32_t v = section.vertexStart; v < sectionEnd; ++v)
                {
                    const uint32_t source = skin->vertexLookup[v];
                    if (source >= header->vertices.count)
                    {
                        ++mappingOutOfRange;
                        continue;
                    }

                    const uint8_t* const record =
                        vertices + static_cast<size_t>(source) * m2::kModelVertexStride;
                    for (uint32_t k = 0; k < 4; ++k)
                    {
                        const uint8_t weight = record[m2::kOffVertexWeights + k];
                        if (!weight) continue;
                        ++positiveInfluences;

                        const uint8_t slot = skin->bones[v * 4 + k];
                        const uint8_t recordSlot = record[m2::kOffVertexBoneSlots + k];
                        if (recordSlot != slot) ++recordSlotMismatches;

                        const uint32_t combo = static_cast<uint32_t>(section.boneComboIndex) + slot;
                        if (slot >= section.boneCount || combo >= header->boneCombos.count)
                        {
                            ++mappingOutOfRange;
                            continue;
                        }

                        const uint16_t global = combos[combo];
                        if (global >= header->bones.count || global >= kProbeBoneCeiling)
                        {
                            ++mappingOutOfRange;
                            continue;
                        }
                        if (seen[global]) continue;
                        seen[global] = true;
                        ++uniqueBones;

                        const float* const matrix = palette + static_cast<size_t>(global) * 16;
                        bool finite = true;
                        bool allZero = true;
                        for (uint32_t e = 0; e < 16; ++e)
                        {
                            if (!std::isfinite(matrix[e])) finite = false;
                            const float a = std::fabs(matrix[e]);
                            if (a > 1.0e-7f) allZero = false;
                            if (a > maxAbsElement) maxAbsElement = a;
                        }
                        if (!finite) ++nonFiniteMatrices;
                        if (allZero) ++zeroMatrices;

                        if (samples < kSampleCount)
                        {
                            sampleGlobal[samples] = global;
                            sampleSkinVertex[samples] = v;
                            sampleSourceVertex[samples] = source;
                            sampleInfluence[samples] = static_cast<uint8_t>(k);
                            sampleWeight[samples] = weight;
                            sampleSkinSlot[samples] = slot;
                            sampleRecordSlot[samples] = recordSlot;
                            sampleSection[samples] = static_cast<uint16_t>(si);
                            ++samples;
                        }
                    }
                }
            }

            uint32_t shaderFlag = 0;
            bool shaderReadable = false;
            __try
            {
                shaderFlag = *reinterpret_cast<volatile const uint32_t*>(m2::kEnableShaders);
                shaderReadable = true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {}

            const auto& firstPart = skin->submeshes[0];
            const auto* const secondPart = sectionParts > 1 ? &skin->submeshes[1] : nullptr;
            WLOG_INFO("orc-female-matrices: ok shader=%s(%#x) instance=%p palette=%p modelBones=%u"
                      " baseId=%u parts=%u vertices=%u windows=%u@%u,%u@%u"
                      " positive=%u unique=%u recordSlotMismatch=%u"
                      " mappingOOB=%u nonFinite=%u allZero=%u maxAbs=%.6g path='%s'",
                      shaderReadable ? (shaderFlag ? "on" : "off") : "unreadable", shaderFlag,
                      instance, palette, header->bones.count, baseSectionId, sectionParts,
                      sectionVertices, firstPart.boneCount, firstPart.boneComboIndex,
                      secondPart ? secondPart->boneCount : 0,
                      secondPart ? secondPart->boneComboIndex : 0,
                      positiveInfluences, uniqueBones,
                      recordSlotMismatches, mappingOutOfRange, nonFiniteMatrices, zeroMatrices,
                      maxAbsElement, path);

            for (uint32_t i = 0; i < samples; ++i)
            {
                const float* const matrix = palette + static_cast<size_t>(sampleGlobal[i]) * 16;
                WLOG_INFO("orc-female-matrix[%u]: part=%u skinVertex=%u sourceVertex=%u influence=%u"
                          " weight=%u gpuSlot=%u recordSlot=%u globalBone=%u"
                          " diag=%.6g,%.6g,%.6g,%.6g translation=%.6g,%.6g,%.6g",
                          i, sampleSection[i], sampleSkinVertex[i], sampleSourceVertex[i],
                          sampleInfluence[i],
                          sampleWeight[i], sampleSkinSlot[i], sampleRecordSlot[i], sampleGlobal[i],
                          matrix[0], matrix[5], matrix[10], matrix[15],
                          matrix[12], matrix[13], matrix[14]);
            }

            // A long single spike is not an authored-index symptom when every bind-pose triangle is
            // compact. Recreate the vertex shader's weighted positions for every live section and keep
            // only the longest animated edges. This names the exact section, vertices, and global bones
            // involved without reading a write-only D3D buffer or mutating the model to guess at a fix.
            struct SkinnedPoint
            {
                float p[3]{};
                uint32_t source = 0;
                uint16_t global[4]{};
                uint8_t weight[4]{};
                uint8_t slot[4]{};
                bool valid = false;
            };
            struct EdgeOutlier
            {
                float length = 0.0f;
                float authoredLength = 0.0f;
                uint32_t section = 0;
                uint32_t triangle = 0;
                uint16_t sectionId = 0;
                uint16_t skinVertex[2]{};
                SkinnedPoint point[2]{};
            };
            constexpr uint32_t kOutlierCount = 12;
            EdgeOutlier outliers[kOutlierCount]{};
            uint32_t outlierMappingOob = 0;
            uint32_t outlierNonFinite = 0;

            auto skinPoint = [&](const wxl::structure::m2::M2SkinSection& section,
                                 uint16_t skinVertex) -> SkinnedPoint
            {
                SkinnedPoint out{};
                if (skinVertex >= skin->vertexCount || skinVertex >= skin->boneCount)
                {
                    ++outlierMappingOob;
                    return out;
                }
                out.source = skin->vertexLookup[skinVertex];
                if (out.source >= header->vertices.count)
                {
                    ++outlierMappingOob;
                    return out;
                }

                const uint8_t* const record =
                    vertices + static_cast<size_t>(out.source) * m2::kModelVertexStride;
                const float* const bind = reinterpret_cast<const float*>(
                    record + m2::kOffVertexPosition);
                float totalWeight = 0.0f;
                for (uint32_t k = 0; k < 4; ++k)
                {
                    out.weight[k] = record[m2::kOffVertexWeights + k];
                    out.slot[k] = skin->bones[static_cast<size_t>(skinVertex) * 4 + k];
                    if (!out.weight[k]) continue;
                    const uint32_t combo = static_cast<uint32_t>(section.boneComboIndex) + out.slot[k];
                    if (out.slot[k] >= section.boneCount || combo >= header->boneCombos.count)
                    {
                        ++outlierMappingOob;
                        return out;
                    }
                    out.global[k] = combos[combo];
                    if (out.global[k] >= header->bones.count)
                    {
                        ++outlierMappingOob;
                        return out;
                    }

                    const float* const matrix =
                        palette + static_cast<size_t>(out.global[k]) * 16;
                    const float w = static_cast<float>(out.weight[k]);
                    out.p[0] += w * (bind[0] * matrix[0] + bind[1] * matrix[4] +
                                      bind[2] * matrix[8] + matrix[12]);
                    out.p[1] += w * (bind[0] * matrix[1] + bind[1] * matrix[5] +
                                      bind[2] * matrix[9] + matrix[13]);
                    out.p[2] += w * (bind[0] * matrix[2] + bind[1] * matrix[6] +
                                      bind[2] * matrix[10] + matrix[14]);
                    totalWeight += w;
                }
                if (totalWeight <= 0.0f) return out;
                for (float& component : out.p) component /= totalWeight;
                out.valid = std::isfinite(out.p[0]) && std::isfinite(out.p[1]) &&
                            std::isfinite(out.p[2]);
                if (!out.valid) ++outlierNonFinite;
                return out;
            };

            auto considerEdge = [&](uint32_t sectionIndex, uint32_t triangle,
                                    uint16_t sectionId, uint16_t aVertex, uint16_t bVertex,
                                    const SkinnedPoint& a, const SkinnedPoint& b)
            {
                if (!a.valid || !b.valid) return;
                const float dx = a.p[0] - b.p[0];
                const float dy = a.p[1] - b.p[1];
                const float dz = a.p[2] - b.p[2];
                const float length = std::sqrt(dx * dx + dy * dy + dz * dz);
                if (!std::isfinite(length) || length <= outliers[kOutlierCount - 1].length)
                    return;

                const auto* const ar = reinterpret_cast<const float*>(
                    vertices + static_cast<size_t>(a.source) * m2::kModelVertexStride +
                    m2::kOffVertexPosition);
                const auto* const br = reinterpret_cast<const float*>(
                    vertices + static_cast<size_t>(b.source) * m2::kModelVertexStride +
                    m2::kOffVertexPosition);
                const float adx = ar[0] - br[0];
                const float ady = ar[1] - br[1];
                const float adz = ar[2] - br[2];

                uint32_t place = kOutlierCount - 1;
                while (place > 0 && length > outliers[place - 1].length)
                {
                    outliers[place] = outliers[place - 1];
                    --place;
                }
                EdgeOutlier& edge = outliers[place];
                edge.length = length;
                edge.authoredLength = std::sqrt(adx * adx + ady * ady + adz * adz);
                edge.section = sectionIndex;
                edge.triangle = triangle;
                edge.sectionId = sectionId;
                edge.skinVertex[0] = aVertex;
                edge.skinVertex[1] = bVertex;
                edge.point[0] = a;
                edge.point[1] = b;
            };

            if (skin->indices)
            {
                const bool extended = bones::UsesExtendedIndexStart(path);
                for (uint32_t si = 0; si < skin->submeshCount; ++si)
                {
                    const auto& section = skin->submeshes[si];
                    const uint32_t start = bones::FullIndexStart(section, extended);
                    if (start > skin->indexCount || section.indexCount > skin->indexCount - start)
                    {
                        ++outlierMappingOob;
                        continue;
                    }
                    for (uint32_t index = 0; index + 2 < section.indexCount; index += 3)
                    {
                        const uint16_t sv[3] = {
                            skin->indices[start + index], skin->indices[start + index + 1],
                            skin->indices[start + index + 2],
                        };
                        const SkinnedPoint point[3] = {
                            skinPoint(section, sv[0]), skinPoint(section, sv[1]),
                            skinPoint(section, sv[2]),
                        };
                        const uint32_t triangle = index / 3;
                        considerEdge(si, triangle, section.skinSectionId,
                                     sv[0], sv[1], point[0], point[1]);
                        considerEdge(si, triangle, section.skinSectionId,
                                     sv[1], sv[2], point[1], point[2]);
                        considerEdge(si, triangle, section.skinSectionId,
                                     sv[2], sv[0], point[2], point[0]);
                    }
                }
            }

            WLOG_INFO("orc-female-edges: sections=%u skinVertices=%u indices=%u mappingOOB=%u"
                      " nonFinite=%u path='%s'", skin->submeshCount, skin->vertexCount,
                      skin->indexCount, outlierMappingOob, outlierNonFinite, path);
            for (uint32_t i = 0; i < kOutlierCount && outliers[i].length > 0.0f; ++i)
            {
                const EdgeOutlier& edge = outliers[i];
                const SkinnedPoint& a = edge.point[0];
                const SkinnedPoint& b = edge.point[1];
                WLOG_INFO("orc-female-edge[%u]: len=%.6g bind=%.6g section=%u id=%u tri=%u"
                          " skin=%u,%u source=%u,%u pos=(%.5g,%.5g,%.5g)->(%.5g,%.5g,%.5g)"
                          " A[w=%u,%u,%u,%u s=%u,%u,%u,%u g=%u,%u,%u,%u]"
                          " B[w=%u,%u,%u,%u s=%u,%u,%u,%u g=%u,%u,%u,%u]",
                          i, edge.length, edge.authoredLength, edge.section, edge.sectionId,
                          edge.triangle, edge.skinVertex[0], edge.skinVertex[1], a.source, b.source,
                          a.p[0], a.p[1], a.p[2], b.p[0], b.p[1], b.p[2],
                          a.weight[0], a.weight[1], a.weight[2], a.weight[3],
                          a.slot[0], a.slot[1], a.slot[2], a.slot[3],
                          a.global[0], a.global[1], a.global[2], a.global[3],
                          b.weight[0], b.weight[1], b.weight[2], b.weight[3],
                          b.slot[0], b.slot[1], b.slot[2], b.slot[3],
                          b.global[0], b.global[1], b.global[2], b.global[3]);
            }
            InterlockedExchange(&g_orcFemaleMatrixProbeState, 2);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            WLOG_WARN("orc-female-matrices: probe fault quarantined instance=%p", instance);
            InterlockedExchange(&g_orcFemaleMatrixProbeState, 2);
        }
    }

    /**
     * @brief Detours bone-palette build, emitting OnBuildBonePalette after the engine fills the buffer.
     *
     * Called from two sites per collection M2 per frame:
     *   (a) the attached-model update path, inside kM2PerFrameUpdate of the parent character.
     *   (b) The outer scene-traversal loop (0x821B4E), which runs AFTER the parent's PerFrameUpdate.
     *
     * Site (b) overwrites any bone-palette modifications that OnM2PerFrameUpdate subscribers made,
     * reverting the collection M2 to its bind pose every frame. By hooking POST-order here,
     * subscribers can re-apply their modifications immediately after the engine's fill -- guaranteed
     * to be the last write before the GPU upload regardless of scene-list ordering.
     *
     * Calling convention: fastcall, ecx = renderCtx, 5 stack args, ret 0x14 (callee-cleanup).
     */
    bool ApplyLampFalloff(void* instance) noexcept
    {
        // CM2Model::AnimateMT owns these per-instance light writes. The native
        // light is embedded at +0x68 in its 0xd4 animated-light record. No node
        // pointers survive the call and no scene-list traversal is involved.
        __try {
            auto* inst = static_cast<unsigned char*>(instance);
            if (!inst) return false;
            auto* shared = *reinterpret_cast<unsigned char**>(inst + m2::kOffInstModel);
            if (!shared) return false;
            const auto* profile = wxl_modern_m2::lamplight::FindProfile(
                reinterpret_cast<const char*>(shared + m2::kOffModelPathStem));
            if (!profile) return false;
            auto* h = *reinterpret_cast<unsigned char**>(shared + m2::kOffModelHeader);
            if (!h || profile->count==0 || profile->count>64 ||
                *reinterpret_cast<uint32_t*>(h+0x108)!=profile->count ||
                *reinterpret_cast<uint32_t*>(h+0x2c)!=profile->bones ||
                *reinterpret_cast<uint32_t*>(h+0x3c)!=profile->vertices) return false;
            auto* record = *reinterpret_cast<unsigned char**>(h+0x10c);
            if (!record) return false;
            using namespace wxl_modern_m2::lamplight;
            auto* animated = *reinterpret_cast<unsigned char**>(inst+0x1d0);
            if (!animated) return false;
            // Validate the whole reviewed light array before making any change.
            for (unsigned i=0;i<profile->count;++i) {
                const auto* r=record+i*0x9c;
                const auto& e=profile->emitters[i];
                const auto* position=reinterpret_cast<const float*>(r+4);
                if (*reinterpret_cast<const uint16_t*>(r)!=1 ||
                    *reinterpret_cast<const uint16_t*>(r+2)!=e.bone ||
                    !Near(position[0],e.position[0]) || !Near(position[1],e.position[1]) ||
                    !Near(position[2],e.position[2]) ||
                    *reinterpret_cast<uint32_t*>(animated+i*0xd4+0x68+8)!=1 ||
                    !OwnedFalloff(reinterpret_cast<float*>(animated+i*0xd4+0x68+0x54))) return false;
            }
            for (unsigned i=0;i<profile->count;++i)
                SetFalloff(reinterpret_cast<float*>(animated+i*0xd4+0x68+0x54));
            return true;
        } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    void __fastcall hkBuildBonePalette(void* renderCtx, void* edx,
        void* sa1, void* sa2, void* sa3, uint32_t sa4, uint32_t sa5)
    {
        g_origBuildBonePalette(renderCtx, edx, sa1, sa2, sa3, sa4, sa5);
        if (ApplyLampFalloff(renderCtx)) {
            static std::atomic<unsigned> reports{0};
            if (reports.load(std::memory_order_relaxed) < 3 &&
                reports.fetch_add(1, std::memory_order_relaxed) < 3)
                WLOG_INFO("lamp-falloff: catalog-reviewed emitter attenuation=(1,0.10,0.005)");
        }
        ev::BuildBonePaletteArgs a{ renderCtx };
        wxl_modern_m2::g_api->Emit(uint32_t(ev::Event::OnBuildBonePalette), &a);
        ProbeFemaleOrcBonePalette(renderCtx);
    }

    /**
     * @brief Detours the M2 ground-shadow batch draw, splitting an over-budget co-instance run into
     *        several native calls instead of drawing it as one.
     *
     * This detour is the ONLY one the client's real M2 ground-shadow draw can carry (MinHook
     * rejects a second on the same target), so the shadow bone probe rides it from here too.
     *
     * The native function's own bone-copy loop (c31-based, 3 registers/bone) is unbounded across the
     * whole co-instance run -- boneCount * coInstanceCount can exceed the 75-bone VS-constant budget
     * even when boneCount alone is small, overflowing past c255 into the device's own vertex-stream
     * slot cache. That overflow is a confirmed, disasm-verified crash: it corrupts a slot record's
     * "count" dword with a bone-matrix float, which FUN_006844c0 later reads as an array index and
     * faults on a wild address (see corpus/re_comprehension/335/m2_instance_0x184_gx_cache.md §14 for
     * the original trace, and the register-level confirmation recorded in this session's own crash
     * triage). Mirrors DrawBatchDoodad's fix exactly, using the run-list shape
     * kShadowRunStride/kShadowRunCountField document: shrink the run's requested-count field to a
     * bone-budget-safe value per sub-call, advance drawIndex by however many co-instances were
     * actually drawn, and restore the field to the original total before returning -- the caller
     * (RenderModelBatchListShadowMap) reads that same field a second time, right after this call
     * returns, to advance its own run cursor.
     */
    void __fastcall hkRenderBatchShadowMap(
        void* instance, void*, uint32_t batchMode, void* skinBatch, void* drawList,
        uint32_t drawIndex, void* skinSection, void* previousSection)
    {
        if (g_shadowBatchMode == 2)
        {
            if (g_shadowBatchLogs.fetch_add(1) < 12)
                WLOG_INFO("shadow-batch-v1: diagnostic hide instance=%p section=%p", instance, skinSection);
            return; // Leave run records intact; the native caller advances past this run normally.
        }
        if constexpr (wxl_modern_m2::kEnabled)
            wxl::runtime::m2shadow::OnShadowBatch(instance, skinSection);

        uint32_t* runs          = nullptr;
        uint32_t  originalCount = 0;
        uint32_t  chunkSize     = 0;
        void* sharedModel = nullptr;
        uint32_t sectionBones = 0;
        __try
        {
            if (drawList && skinSection)
            {
                auto* listData = *reinterpret_cast<uint32_t* const*>(drawList);
                if (listData)
                {
                    runs = listData;
                    originalCount = runs[drawIndex * m2::kShadowRunStride + m2::kShadowRunCountField];

                    const uint32_t boneCount =
                        static_cast<const wxl::structure::m2::M2SkinSection*>(skinSection)->boneCount;
                    sectionBones = boneCount;
                    if (instance)
                        sharedModel = reinterpret_cast<void*>(static_cast<m2::M2Instance*>(instance)->model);
                    chunkSize = boneCount > 0
                        ? std::max<uint32_t>(1u, bones::kMaxBonesPerDraw / boneCount)
                        : originalCount;
                    // Diagnostic: preserve the native loop/cursor, but grant one instance for
                    // native-modern extended-index models. Other shadow paths remain unchanged.
                    if (g_singleShadowTest && sharedModel &&
                        wxl::modern::assets::m2::IsNativeLoaded(sharedModel))
                    {
                        const char* path = wxl::game::m2::M2Model(sharedModel).GetPathStem();
                        if (path && bones::UsesExtendedIndexStart(path))
                        {
                            chunkSize = 1;
                            if (g_singleShadowLogs.fetch_add(1) < 48)
                                WLOG_INFO("shadow-single-v1: path='%s' count=%u bones=%u capacity=1",path,originalCount,boneCount);
                        }
                    }
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            runs = nullptr; // fail safe: single native call below, run list left untouched
        }

        if (g_shadowBatchMode == 1 && runs && sharedModel && originalCount)
        {
            // A single over-budget section must be fixed at skin finalization, not multiplied
            // into an out-of-bounds native constants upload. Decline only that unsafe shadow.
            if (sectionBones > bones::kMaxBonesPerDraw)
            {
                if (g_shadowBatchLogs.fetch_add(1) < 12)
                    WLOG_WARN("shadow-batch-v1: skipped over-budget section instance=%p bones=%u count=%u",
                              instance, sectionBones, originalCount);
                return;
            }
            if (originalCount > chunkSize && g_shadowBatchLogs.fetch_add(1) < 24)
                WLOG_INFO("shadow-batch-v1: native loop instance=%p shared=%p section=%p count=%u bones=%u cap=%u previous=%p",
                          instance, sharedModel, skinSection, originalCount, sectionBones, chunkSize, previousSection);
            DrawShadowWithCapacity(sharedModel, chunkSize, instance, batchMode, skinBatch,
                                   drawList, drawIndex, skinSection, previousSection);
            return;
        }

        if (!runs || chunkSize >= originalCount)
        {
            g_origRenderBatchShadowMap(instance, nullptr, batchMode, skinBatch, drawList,
                                       drawIndex, skinSection, previousSection);
            return;
        }

        uint32_t drawn = 0;
        while (drawn < originalCount)
        {
            const uint32_t thisChunk = std::min(chunkSize, originalCount - drawn);
            const uint32_t thisIndex = drawIndex + drawn;
            runs[thisIndex * m2::kShadowRunStride + m2::kShadowRunCountField] = thisChunk;
            g_origRenderBatchShadowMap(instance, nullptr, batchMode, skinBatch, drawList,
                                       thisIndex, skinSection, previousSection);
            drawn += thisChunk;
        }
        runs[drawIndex * m2::kShadowRunStride + m2::kShadowRunCountField] = originalCount;
    }

    /**
     * @brief Detours the main-draw batched-doodad path, splitting an over-budget co-instance batch into
     *        several native calls instead of drawing it as one.
     *
     * The native function already loops internally over groups of AllocInstances' granted capacity, but
     * that capacity is sized for GPU buffer space, not for the c31-based VS-constant budget -- a group
     * can still ask for more than kMaxBonesPerDraw total bones across its co-instances. The fix mirrors
     * that same internal loop shape from the outside: shrink the batch record's run-length field
     * (kM2ElementRunLengthField) to a bone-budget-safe count per call, advance the indices pointer by
     * what was actually drawn, and restore the field to its original value before returning -- the
     * caller (CM2SceneRender::Draw) reads that same field a second time, right after this call returns,
     * to advance its own sorted-index cursor past the whole run.
     */
    void __fastcall hkDrawBatchDoodad(void* ctx, void* edx, void* elements, void* indices)
    {
        uint32_t* countField    = nullptr;
        uint32_t  originalCount = 0;
        uint32_t  chunkSize     = 0;
        __try
        {
            auto* c = static_cast<gxoff::DrawBatchContext*>(ctx);
            if (c->element)
            {
                auto* elementBytes = static_cast<uint8_t*>(c->element);
                countField    = reinterpret_cast<uint32_t*>(elementBytes + gxoff::kM2ElementRunLengthField);
                originalCount = *countField;

                const void* section =
                    *reinterpret_cast<void* const*>(elementBytes + gxoff::kM2ElementSectionField);
                const uint32_t boneCount = section
                    ? static_cast<const wxl::structure::m2::M2SkinSection*>(section)->boneCount
                    : 0;

                chunkSize = boneCount > 0
                    ? std::max<uint32_t>(1u, bones::kMaxBonesPerDraw / boneCount)
                    : originalCount;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            countField = nullptr; // fail safe: single native call below, batch record left untouched
        }

        if (!countField || chunkSize >= originalCount)
        {
            g_origDrawBatchDoodad(ctx, edx, elements, indices);
            return;
        }

        auto*    indexBytes = static_cast<uint8_t*>(indices);
        uint32_t drawn       = 0;
        while (drawn < originalCount)
        {
            const uint32_t thisChunk = std::min(chunkSize, originalCount - drawn);
            *countField = thisChunk;
            g_origDrawBatchDoodad(ctx, edx, elements, indexBytes + static_cast<size_t>(drawn) * 4);
            drawn += thisChunk;
        }
        *countField = originalCount;
    }
}

namespace wxl_modern_m2
{
    bool InstallM2CompatBones()
    {
        g_shadowBatchMode = ConfigU32("WXL_M2_SHADOW_BATCH_MODE", 1, 0, 2);
        const bool allocatorHooked = HookAttachByName("M2.SharedAllocInstances",
            &hkSharedAllocInstances, &g_origSharedAllocInstances);
        if (g_shadowBatchMode == 1 && !allocatorHooked)
        {
            WLOG_WARN("shadow-batch-v1: capacity hook unavailable; retaining historical splitter");
            g_shadowBatchMode = 0;
        }
        g_singleShadowTest = wxl_modern_m2::ConfigBool("WXL_M2_SHADOW_SINGLE_INSTANCE_TEST",false);
        WLOG_INFO("shadow-single-v1: enabled=%u",g_singleShadowTest?1u:0u);
        WLOG_INFO("shadow-batch-v1: mode=%u allocatorHook=%u (0=historical 1=native-capacity 2=hide)",
                  g_shadowBatchMode, allocatorHooked ? 1u : 0u);
        HookAttachByName("M2.BuildBonePalette", &hkBuildBonePalette, &g_origBuildBonePalette);
        const bool shadowHooked = HookAttachByName("M2.RenderBatchShadowMap",
                                                    &hkRenderBatchShadowMap, &g_origRenderBatchShadowMap);
        HookAttachByName("M2.DrawBatchDoodad", &hkDrawBatchDoodad, &g_origDrawBatchDoodad);
        if constexpr (kEnabled)
            wxl::runtime::m2shadow::Arm(shadowHooked);
        return true;
    }
}
