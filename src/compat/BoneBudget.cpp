// Bone budget: partition a submesh whose per-draw bone palette exceeds the client ceiling into
// sub-sections, and re-point batches across the resulting sub-section run.
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

#include "BoneBudget.hpp"
#include "VertexOrigin.hpp"

#include "../ExtensionApi.hpp"

#include "engine/assets/shared/common/Text.hpp"
#include "offsets/game/M2.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace wxl::modern::assets::common::bones
{
    namespace fmt  = wxl::structure::m2;
    namespace off  = wxl::offsets::game::m2;
    namespace text = wxl::modern::assets::common::text;
    using Skin     = wxl::game::m2::M2SkinProfile;

    namespace
    {
        constexpr uint32_t kSkinU16Max         = 0xFFFF;  // u16 ceiling of the skin arrays
        constexpr uint32_t kSplitMaxBoneCombos = 0x10000; // boneCombos upper bound, rejected before bulk copy
        constexpr uint32_t kVertexStride       = 0x30;    // one M2 vertex record
        constexpr uint32_t kOffVertexWeights   = 0x0C;    // uint8[4]: influence weights
        constexpr uint32_t kOffVertexBoneIdx   = 0x10;    // uint8[4]: the register slots the shader reads

        /// Everything the finalize overwrites that the palette upload still needs. Kept per rebuilt
        /// header, replaced whenever the same header is rebuilt again.
        struct PaletteRestore
        {
            std::vector<uint16_t> boneMap;    ///< what boneCombos has to read back as
            std::vector<uint16_t> comboIndex; ///< each rebuilt section's window start
        };
        std::unordered_map<const fmt::M2Header*, PaletteRestore> g_paletteMaps;
        std::unordered_map<const fmt::M2Header*, uint8_t*> g_stableCharacterVertices;
        std::unordered_map<const fmt::M2Header*, wxl_modern_m2::vertexorigin::Origin> g_vertexOrigins;
        std::mutex g_vertexOriginMutex;

        bool FindVertexOrigin(const fmt::M2Header* header, wxl_modern_m2::vertexorigin::Origin& out)
        {
            const std::lock_guard<std::mutex> lock(g_vertexOriginMutex);
            const auto found = g_vertexOrigins.find(header);
            if (found == g_vertexOrigins.end()) return false;
            out = found->second;
            return true;
        }

        enum class StableVertexTarget : uint8_t
        {
            None,
            OrcMale,
            OrcFemale,
            ScourgeMale,
            ScourgeFemale,
            TaurenMale,
            TaurenFemale,
            VulperaMale,
            VulperaFemale,
            BloodElfMale,
            BloodElfFemale,
            NightborneMale,
            NightborneFemale,
            HumanMale,
            HumanFemale,
            DwarfMale,
            DwarfFemale,
            NightElfMale,
            NightElfFemale,
            GnomeMale,
            GnomeFemale,
            DraeneiMale,
            DraeneiFemale,
            VoidElfMale,
            VoidElfFemale,
            PandarenFemale,
            KulTiranMale,
            KulTiranFemale,
            ZandalariMale,
            ZandalariFemale,
            GoblinMale,
        };

        bool IsExactModelPath(std::string_view path, std::string_view stem)
        {
            return (path.size() == stem.size() && text::StartsWithCI(path, stem)) ||
                   (path.size() == stem.size() + 3 && text::StartsWithCI(path, stem) &&
                    text::StartsWithCI(path.substr(stem.size()), ".m2"));
        }

        StableVertexTarget StableVertexTargetFor(std::string_view path)
        {
            if (IsExactModelPath(path, "character\\orc\\male\\orcmale_hd"))
                return StableVertexTarget::OrcMale;
            if (IsExactModelPath(path, "character\\orc\\female\\orcfemale_hd"))
                return StableVertexTarget::OrcFemale;
            if (IsExactModelPath(path, "character\\scourge\\male\\scourgemale_hd"))
                return StableVertexTarget::ScourgeMale;
            if (IsExactModelPath(path, "character\\scourge\\female\\scourgefemale_hd"))
                return StableVertexTarget::ScourgeFemale;
            if (IsExactModelPath(path, "character\\tauren\\male\\taurenmale_hd"))
                return StableVertexTarget::TaurenMale;
            if (IsExactModelPath(path, "character\\tauren\\female\\taurenfemale_hd"))
                return StableVertexTarget::TaurenFemale;
            if (IsExactModelPath(path, "character\\vulpera\\male\\vulperamale"))
                return StableVertexTarget::VulperaMale;
            if (IsExactModelPath(path, "character\\vulpera\\female\\vulperafemale"))
                return StableVertexTarget::VulperaFemale;
            if (IsExactModelPath(path, "character\\bloodelf\\male\\bloodelfmale_hd") ||
                IsExactModelPath(path, "hd\\character\\bloodelf\\male\\bloodelfmale"))
                return StableVertexTarget::BloodElfMale;
            if (IsExactModelPath(path, "character\\bloodelf\\female\\bloodelffemale_hd") ||
                IsExactModelPath(path, "hd\\character\\bloodelf\\female\\bloodelffemale"))
                return StableVertexTarget::BloodElfFemale;
            if (IsExactModelPath(path, "character\\nightborne\\male\\nightbornemale"))
                return StableVertexTarget::NightborneMale;
            if (IsExactModelPath(path, "character\\nightborne\\female\\nightbornefemale"))
                return StableVertexTarget::NightborneFemale;
            if (IsExactModelPath(path, "character\\human\\male\\humanmale_hd"))
                return StableVertexTarget::HumanMale;
            if (IsExactModelPath(path, "character\\human\\female\\humanfemale_hd"))
                return StableVertexTarget::HumanFemale;
            if (IsExactModelPath(path, "character\\dwarf\\male\\dwarfmale_hd"))
                return StableVertexTarget::DwarfMale;
            if (IsExactModelPath(path, "character\\dwarf\\female\\dwarffemale_hd"))
                return StableVertexTarget::DwarfFemale;
            if (IsExactModelPath(path, "character\\darkirondwarf\\male\\darkirondwarfmale"))
                return StableVertexTarget::DwarfMale;
            if (IsExactModelPath(path, "character\\darkirondwarf\\female\\darkirondwarffemale"))
                return StableVertexTarget::DwarfFemale;
            if (IsExactModelPath(path, "character\\nightelf\\male\\nightelfmale_hd"))
                return StableVertexTarget::NightElfMale;
            if (IsExactModelPath(path, "character\\nightelf\\female\\nightelffemale_hd"))
                return StableVertexTarget::NightElfFemale;
            if (IsExactModelPath(path, "character\\gnome\\male\\gnomemale_hd"))
                return StableVertexTarget::GnomeMale;
            if (IsExactModelPath(path, "character\\gnome\\female\\gnomefemale_hd"))
                return StableVertexTarget::GnomeFemale;
            if (IsExactModelPath(path, "character\\draenei\\male\\draeneimale_hd") ||
                IsExactModelPath(path, "hd\\character\\draenei\\male\\draeneimale"))
                return StableVertexTarget::DraeneiMale;
            if (IsExactModelPath(path, "character\\draenei\\female\\draeneifemale_hd") ||
                IsExactModelPath(path, "hd\\character\\draenei\\female\\draeneifemale") ||
                IsExactModelPath(path,
                                 "character\\lightforgeddraenei\\female\\lightforgeddraeneifemale"))
                return StableVertexTarget::DraeneiFemale;
            if (IsExactModelPath(path, "character\\voidelf\\male\\voidelfmale"))
                return StableVertexTarget::VoidElfMale;
            if (IsExactModelPath(path, "character\\voidelf\\female\\voidelffemale"))
                return StableVertexTarget::VoidElfFemale;
            if (IsExactModelPath(path, "character\\pandaren\\female\\pandafemale") ||
                IsExactModelPath(path, "character\\pandaren\\female\\pandarenfemale"))
                return StableVertexTarget::PandarenFemale;
            if (IsExactModelPath(path, "character\\naga_\\male\\kultiranmale") ||
                IsExactModelPath(path, "character\\kultiran\\male\\kultiranmale"))
                return StableVertexTarget::KulTiranMale;
            if (IsExactModelPath(path, "character\\naga_\\female\\kultiranfemale") ||
                IsExactModelPath(path, "character\\kultiran\\female\\kultiranfemale"))
                return StableVertexTarget::KulTiranFemale;
            if (IsExactModelPath(path,
                                 "character\\zandalaritroll\\male\\zandalaritrollmale"))
                return StableVertexTarget::ZandalariMale;
            if (IsExactModelPath(path,
                                 "character\\zandalaritroll\\female\\zandalaritrollfemale"))
                return StableVertexTarget::ZandalariFemale;
            if (IsExactModelPath(path, "character\\goblin\\male\\goblinm") ||
                IsExactModelPath(path, "character\\goblin\\male\\goblinmale"))
                return StableVertexTarget::GoblinMale;
            return StableVertexTarget::None;
        }

        /** @brief Keeps the forced split and palette diagnostic on the one active Retail character canary. */
        bool IsFemaleOrcCanary(std::string_view path)
        {
            return StableVertexTargetFor(path) == StableVertexTarget::OrcFemale;
        }

        struct SplitPolicy
        {
            uint16_t maxBonesPerDraw;
            bool forceGeometry;
        };

        /** @brief Uses the stock-compatible window compaction without rebuilding model topology. */
        SplitPolicy PolicyFor(std::string_view /*path*/)
        {
            return SplitPolicy{ kMaxBonesPerDraw, false };
        }

        /** @brief Reads the fixed-build programmable-renderer flag without letting a probe fault the client. */
        bool ReadShaderPathFlag(uint32_t& value) noexcept
        {
            __try
            {
                value = *reinterpret_cast<volatile const uint32_t*>(off::kEnableShaders);
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                value = 0;
                return false;
            }
        }

        const char* ShaderPathName(bool readable, uint32_t value)
        {
            return !readable ? "unreadable" : (value ? "shader" : "cpu");
        }

        /**
         * @brief Retains a stable vertex array for character models whose first record is overwritten.
         *
         * The mounted M2 carries a sane vertex zero, but the first twelve bytes of its live source
         * array later become allocator-like garbage while the weights and the rest of the array stay
         * intact. Repairing the position at this point is too early: the backing storage is damaged
         * after this pass. Give the header model-lifetime storage of its own instead, before the
         * vertex-buffer fill and before CompactBoneWindows stamps the per-section bone slots.
         *
         * Human male exhibits the same failure, and Human female can acquire it after a sex/model
         * transition. Keep this exact-path and one-copy-per-header guarded. The model arrays rebuilt
         * elsewhere in this compatibility path already follow the same model-lifetime allocation policy.
         */
        void StabilizeCharacterVertices(fmt::M2Header* md, Skin* skin, std::string_view path)
        {
            const StableVertexTarget target = StableVertexTargetFor(path);
            wxl_modern_m2::vertexorigin::Origin origin{};
            const bool sourceOwned = md && FindVertexOrigin(md, origin) &&
                wxl_modern_m2::vertexorigin::Matches(origin, md->vertices.count);
            // Provenance is the validated modern native load, not merely a legacy-looking filename.
            const bool nativeCharacter = sourceOwned &&
                (text::StartsWithCI(path, "character\\") || text::StartsWithCI(path, "hd\\character\\"));
            if (!md || !md->vertices.count || !md->vertices.offset ||
                (target == StableVertexTarget::None && !nativeCharacter))
                return;

            const auto found = g_stableCharacterVertices.find(md);
            if (found != g_stableCharacterVertices.end()
                && md->vertices.offset == static_cast<uint32_t>(reinterpret_cast<uintptr_t>(found->second)))
                return;

            if (md->vertices.count > SIZE_MAX / kVertexStride)
                return;

            const size_t bytes = static_cast<size_t>(md->vertices.count) * kVertexStride;
            auto* const source = reinterpret_cast<const uint8_t*>(
                static_cast<uintptr_t>(md->vertices.offset));
            auto* const stable = static_cast<uint8_t*>(std::malloc(bytes));
            if (!stable)
                return;

            std::memcpy(stable, source, bytes);
            auto* const position = reinterpret_cast<float*>(stable);
            const auto damaged = [](float value) {
                return !std::isfinite(value) || std::fabs(value) > 1000.0f;
            };
            if (damaged(position[0]) || damaged(position[1]) || damaged(position[2]))
            {
                const float oldX = position[0], oldY = position[1], oldZ = position[2];
                if (sourceOwned)
                {
                    // An HD model and its non-HD Retail counterpart can have different vertex zero.
                    // Recover from this exact header's source bytes, never another family's constant.
                    std::memcpy(position, origin.position, sizeof origin.position);
                }
                else if (target == StableVertexTarget::OrcMale)
                {
                    // Exact vertex zero from the deployed Retail M2 body (FDID 917116).
                    position[0] = -0.06169989f;
                    position[1] = -0.6910669f;
                    position[2] = 0.8234006f;
                }
                else if (target == StableVertexTarget::OrcFemale)
                {
                    position[0] = 0.0358651690f;
                    position[1] = -0.4418407977f;
                    position[2] = 0.9912713170f;
                }
                else if (target == StableVertexTarget::ScourgeMale)
                {
                    // Exact vertex zero from the deployed Retail M2 body (FDID 959310).
                    position[0] = 0.1085561f;
                    position[1] = -0.7675533f;
                    position[2] = 1.222004f;
                }
                else if (target == StableVertexTarget::ScourgeFemale)
                {
                    // Exact vertex zero from the deployed Retail M2 body (FDID 997378).
                    position[0] = 0.042884f;
                    position[1] = 0.6825027f;
                    position[2] = 1.099903f;
                }
                else if (target == StableVertexTarget::TaurenMale)
                {
                    // Exact vertex zero from the staged Retail M2 body (FDID 968705).
                    position[0] = 0.5995167f;
                    position[1] = 0.4944605f;
                    position[2] = 0.0000003667502f;
                }
                else if (target == StableVertexTarget::TaurenFemale)
                {
                    // Exact vertex zero from the staged Retail M2 body (FDID 986648).
                    position[0] = -0.07675332f;
                    position[1] = 0.4377006f;
                    position[2] = -0.003803857f;
                }
                else if (target == StableVertexTarget::VulperaMale)
                {
                    // Exact vertex zero from the staged Retail M2 body (FDID 1890761).
                    position[0] = -0.1936546f;
                    position[1] = -0.05097233f;
                    position[2] = 0.5856579f;
                }
                else if (target == StableVertexTarget::VulperaFemale)
                {
                    // Exact vertex zero from the staged Retail M2 body (FDID 1890762).
                    position[0] = -0.1115783f;
                    position[1] = 0.2109648f;
                    position[2] = 0.007011574f;
                }
                else if (target == StableVertexTarget::BloodElfMale)
                {
                    // Exact vertex zero from the staged Retail M2 body (ChrModel 1100087).
                    position[0] = -0.04583823f;
                    position[1] = 0.06183374f;
                    position[2] = 1.889943f;
                }
                else if (target == StableVertexTarget::BloodElfFemale)
                {
                    // Exact vertex zero from the staged Retail M2 body (ChrModel 1100258).
                    position[0] = -0.01798389f;
                    position[1] = -0.006946617f;
                    position[2] = 1.396202f;
                }
                else if (target == StableVertexTarget::NightborneMale)
                {
                    // Exact vertex zero from the staged Retail M2 body (ChrModel 1810675).
                    position[0] = -0.05554726f;
                    position[1] = -0.5224726f;
                    position[2] = 1.18172f;
                }
                else if (target == StableVertexTarget::NightborneFemale)
                {
                    // Exact vertex zero from the staged Retail M2 body (ChrModel 1810676).
                    position[0] = -0.03040518f;
                    position[1] = -0.7821534f;
                    position[2] = 1.459329f;
                }
                else if (target == StableVertexTarget::HumanMale)
                {
                    position[0] = -0.1395796f;
                    position[1] = 0.2305281f;
                    position[2] = 1.473176f;
                }
                else if (target == StableVertexTarget::HumanFemale)
                {
                    position[0] = -0.05357748f;
                    position[1] = 0.6159261f;
                    position[2] = 1.105453f;
                }
                else if (target == StableVertexTarget::DwarfMale)
                {
                    position[0] = -0.067421935f;
                    position[1] = -0.3921781f;
                    position[2] = 0.58857554f;
                }
                else if (target == StableVertexTarget::DwarfFemale)
                {
                    position[0] = -0.1258083f;
                    position[1] = 0.6270641f;
                    position[2] = 0.7849417f;
                }
                else if (target == StableVertexTarget::NightElfFemale)
                {
                    position[0] = -0.10304014f;
                    position[1] = -0.28812778f;
                    position[2] = 1.9055536f;
                }
                else if (target == StableVertexTarget::NightElfMale)
                {
                    position[0] = -0.1570361f;
                    position[1] = -0.36763403f;
                    position[2] = 2.030115f;
                }
                else if (target == StableVertexTarget::GnomeMale)
                {
                    position[0] = -0.18060529f;
                    position[1] = 0.29535815f;
                    position[2] = 0.5672884f;
                }
                else if (target == StableVertexTarget::GnomeFemale)
                {
                    position[0] = -0.15542877f;
                    position[1] = 0.23319131f;
                    position[2] = 0.62551075f;
                }
                else if (target == StableVertexTarget::DraeneiMale)
                {
                    position[0] = 0.100203425f;
                    position[1] = -0.116097055f;
                    position[2] = 0.07345829f;
                }
                else if (target == StableVertexTarget::VoidElfMale)
                {
                    position[0] = -0.0847798586f;
                    position[1] = 0.327067196f;
                    position[2] = 1.73303211f;
                }
                else if (target == StableVertexTarget::VoidElfFemale)
                {
                    position[0] = -0.0912052542f;
                    position[1] = 0.223552629f;
                    position[2] = 1.55758119f;
                }
                else if (target == StableVertexTarget::PandarenFemale)
                {
                    position[0] = -0.2133164f;
                    position[1] = 0.06918611f;
                    position[2] = 1.15505f;
                }
                else if (target == StableVertexTarget::KulTiranMale)
                {
                    // Exact vertex zero from the deployed Retail M2 body (FDID 1721003).
                    position[0] = -0.1500395f;
                    position[1] = 1.001591f;
                    position[2] = 1.342525f;
                }
                else if (target == StableVertexTarget::KulTiranFemale)
                {
                    // Exact vertex zero from the deployed Retail M2 body (FDID 1886724).
                    position[0] = -0.03397197f;
                    position[1] = 0.7630178f;
                    position[2] = 1.3215f;
                }
                else if (target == StableVertexTarget::ZandalariMale)
                {
                    // Exact vertex zero from the staged Retail Zandalari male M2 (FDID 1630447).
                    position[0] = -0.2172331f;
                    position[1] = -0.1964313f;
                    position[2] = 0.05646672f;
                }
                else if (target == StableVertexTarget::ZandalariFemale)
                {
                    // Exact vertex zero from the staged Retail Zandalari female M2 (FDID 1662187).
                    position[0] = -0.009341252f;
                    position[1] = -0.0987683f;
                    position[2] = 0.02226912f;
                }
                else if (target == StableVertexTarget::GoblinMale)
                {
                    // Exact vertex zero from the staged Retail Goblin male M2 (FDID 119376).
                    position[0] = -0.1655005f;
                    position[1] = 0.2625775f;
                    position[2] = 0.9144554f;
                }
                else
                {
                    position[0] = 0.083329f;
                    position[1] = -0.2916879f;
                    position[2] = 0.0014590948f;
                }
                WLOG_WARN("character-vertex-stable: path='%.*s' source vertex=0 was already"
                          " damaged old=(%.9g,%.9g,%.9g) restored=(%.9g,%.9g,%.9g)",
                          static_cast<int>(path.size()), path.data(), oldX, oldY, oldZ,
                          position[0], position[1], position[2]);
            }

            // Pandaren female's two tail sections are authored against the replaceable body sheet,
            // but their UVs sit over the lower-left foot/toe-pad cells. On this client that makes the
            // long tail wear the paw-pad pattern like a face. Retail supplies no tail material or
            // alternate UV set for either choice: both 3801 (nub) and 3802 (long) use texture type 1
            // and UV1 is identically zero. Move only those vertices' V footprint into the matching
            // clean body-fur cells of the same selected skin atlas. The stable copy keeps the source
            // model immutable and makes the correction model-lifetime, like the spike safeguard.
            if (target == StableVertexTarget::PandarenFemale && skin && skin->submeshes &&
                skin->vertexLookup)
            {
                std::vector<uint8_t> adjusted(md->vertices.count, 0);
                uint32_t count = 0;
                for (uint32_t si = 0; si < skin->submeshCount; ++si)
                {
                    const fmt::M2SkinSection& section = skin->submeshes[si];
                    if (section.skinSectionId != 3801 && section.skinSectionId != 3802) continue;
                    const uint32_t end = static_cast<uint32_t>(section.vertexStart) +
                        section.vertexCount;
                    if (end > skin->vertexCount) continue;
                    for (uint32_t v = section.vertexStart; v < end; ++v)
                    {
                        const uint32_t sourceIndex = skin->vertexLookup[v];
                        if (sourceIndex >= md->vertices.count || adjusted[sourceIndex]) continue;
                        adjusted[sourceIndex] = 1;
                        float* const uv = reinterpret_cast<float*>(
                            stable + static_cast<size_t>(sourceIndex) * kVertexStride + 0x20);
                        if (std::isfinite(uv[1]) && uv[1] >= 0.65f)
                        {
                            uv[1] -= 0.65f;
                            ++count;
                        }
                    }
                }
                WLOG_INFO("pandaren-female-tail-uv: moved %u vertex/vertices to body-fur atlas cells",
                          count);
            }

            // Tauren neck and Vulpera tail/rear-head UVs retain their authored
            // lower-left atlas coordinates. CharacterLayers restores that cell
            // from the selected base skin after legacy composition; relocating
            // these UVs into arm/clothing cells made gear repaint the appendages.

            const uint32_t oldAddress = md->vertices.offset;
            md->vertices.offset = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(stable));
            g_stableCharacterVertices[md] = stable;
            if (nativeCharacter)
                WLOG_INFO("vertex-origin-v1: path='%.*s' vertices=%u source0=%.7g,%.7g,%.7g target=%u",
                          static_cast<int>(path.size()), path.data(), md->vertices.count,
                          origin.position[0], origin.position[1], origin.position[2],
                          static_cast<unsigned>(target));
            WLOG_INFO("character-vertex-stable: path='%.*s' retained %u vertices (%u bytes)"
                      " old=%08X new=%08X", static_cast<int>(path.size()), path.data(),
                      md->vertices.count, static_cast<uint32_t>(bytes), oldAddress,
                      md->vertices.offset);
        }
    }

    namespace
    {
        /**
         * @brief Renumbers every section's bone window in place, touching no geometry at all.
         *
         * A section's vertices are its own: the sections partition the vertex list, so no vertex is
         * ever claimed twice and each can carry one register slot without anybody else disagreeing
         * about it. That is what makes renumbering a rewrite of four bytes per vertex rather than a
         * rebuild of the model, and it is why this path exists: the split below reorders vertices and
         * re-emits indices, which is a great deal of motion to buy nothing when no section is actually
         * over the ceiling.
         *
         * The slots are stamped into the vertex records too so the raw records and the live skin-property
         * array agree regardless of which native vertex-build path consumes them.
         * @return true when every section fit and the model is finished here; false when at least one
         *         needs more bones than one draw can hold, which only splitting geometry can answer.
         */
        bool CompactBoneWindows(fmt::M2Header* md, Skin* skin, const uint16_t* boneCombos,
                                uint32_t boneComboCount, uint16_t maxBonesPerDraw,
                                std::vector<SplitRun>& splitMap, const char* name)
        {
            auto* const verts = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(md->vertices.offset));
            if (!verts || !md->vertices.count) return false;

            const auto boneAt = [&](const fmt::M2SkinSection& s, uint32_t vertex, uint32_t k) -> uint16_t
            {
                const uint32_t combo = s.boneComboIndex + skin->bones[vertex * 4 + k];
                return combo < boneComboCount ? boneCombos[combo] : 0;
            };

            std::vector<std::vector<uint16_t>> windows(skin->submeshCount);
            for (uint32_t si = 0; si < skin->submeshCount; ++si)
            {
                const fmt::M2SkinSection& s = skin->submeshes[si];
                if (static_cast<uint32_t>(s.vertexStart) + s.vertexCount > skin->vertexCount) return false;

                std::vector<uint16_t>& window = windows[si];
                window.reserve(static_cast<size_t>(s.vertexCount) * 4);
                for (uint32_t v = s.vertexStart; v < static_cast<uint32_t>(s.vertexStart) + s.vertexCount; ++v)
                    for (uint32_t k = 0; k < 4; ++k) window.push_back(boneAt(s, v, k));

                std::sort(window.begin(), window.end());
                window.erase(std::unique(window.begin(), window.end()), window.end());
                if (window.size() > maxBonesPerDraw) return false;
            }

            std::vector<uint16_t> slotCombos(boneCombos, boneCombos + boneComboCount);
            std::vector<uint16_t> boneMap(boneCombos, boneCombos + boneComboCount);
            std::vector<uint16_t> comboIndex(skin->submeshCount, 0);

            // Skin vertex ranges are section-local, but their vertexLookup entries are allowed to
            // reuse one model vertex across several sections. The compact path also stamps local
            // slots into that shared model record for native finalize. If two sections number the
            // weighted influences differently, one four-byte record cannot represent both: the last
            // section wins and an earlier triangle is skinned to a pelvis/root bone, producing the
            // screen-length arm/hand/shoulder spikes seen across Human, Dwarf, Night Elf and Gnome.
            // Detect that contract before mutating anything and let SplitSubmeshes duplicate the
            // source records, which is precisely the representation this case requires.
            std::vector<std::array<uint8_t, 4>> sourceSlots(md->vertices.count);
            std::vector<uint8_t> sourceAssigned(md->vertices.count, 0);
            for (uint32_t si = 0; si < skin->submeshCount; ++si)
            {
                const fmt::M2SkinSection& s = skin->submeshes[si];
                const std::vector<uint16_t>& window = windows[si];
                for (uint32_t v = s.vertexStart;
                     v < static_cast<uint32_t>(s.vertexStart) + s.vertexCount; ++v)
                {
                    const uint32_t source = skin->vertexLookup[v];
                    if (source >= md->vertices.count) return false;
                    const uint8_t* record =
                        verts + static_cast<size_t>(source) * kVertexStride;
                    std::array<uint8_t, 4> planned{};
                    for (uint32_t k = 0; k < 4; ++k)
                    {
                        const uint16_t bone = boneAt(s, v, k);
                        const auto at = std::lower_bound(window.begin(), window.end(), bone);
                        planned[k] = static_cast<uint8_t>(
                            at != window.end() && *at == bone ? at - window.begin() : 0);
                    }
                    if (sourceAssigned[source])
                    {
                        bool conflict = false;
                        for (uint32_t k = 0; k < 4; ++k)
                            if (record[kOffVertexWeights + k] &&
                                sourceSlots[source][k] != planned[k])
                                conflict = true;
                        if (conflict)
                        {
                            WLOG_INFO("modern-assets: '%s' shares source vertex %u across incompatible bone windows; rebuilding geometry",
                                      name ? name : "", source);
                            return false;
                        }
                    }
                    else
                    {
                        sourceSlots[source] = planned;
                        sourceAssigned[source] = 1;
                    }
                }
            }

            auto* const bones = static_cast<uint8_t*>(std::malloc(static_cast<size_t>(skin->vertexCount) * 4));
            if (!bones) return false;
            std::memcpy(bones, skin->bones, static_cast<size_t>(skin->vertexCount) * 4);

            for (uint32_t si = 0; si < skin->submeshCount; ++si)
            {
                const fmt::M2SkinSection& s = skin->submeshes[si];
                const std::vector<uint16_t>& window = windows[si];
                if (window.empty()) continue;

                comboIndex[si] = static_cast<uint16_t>(boneMap.size());
                for (uint16_t slot = 0; slot < window.size(); ++slot)
                {
                    boneMap.push_back(window[slot]);
                    slotCombos.push_back(slot);
                }

                for (uint32_t v = s.vertexStart; v < static_cast<uint32_t>(s.vertexStart) + s.vertexCount; ++v)
                {
                    const uint32_t source = skin->vertexLookup[v];
                    uint8_t* record = source < md->vertices.count
                                    ? verts + static_cast<size_t>(source) * kVertexStride : nullptr;
                    for (uint32_t k = 0; k < 4; ++k)
                    {
                        const uint16_t bone = boneAt(s, v, k);
                        const auto at = std::lower_bound(window.begin(), window.end(), bone);
                        const auto slot = static_cast<uint8_t>(
                            at != window.end() && *at == bone ? at - window.begin() : 0);
                        bones[v * 4 + k] = slot;
                        if (record) record[kOffVertexBoneIdx + k] = slot;
                    }
                }
            }

            if (boneMap.size() > kSkinU16Max)
            {
                std::free(bones);
                return false;
            }

            auto* const combos = static_cast<uint16_t*>(std::malloc(slotCombos.size() * sizeof(uint16_t)));
            if (!combos)
            {
                std::free(bones);
                return false;
            }
            std::memcpy(combos, slotCombos.data(), slotCombos.size() * sizeof(uint16_t));

            uint32_t widest = 1;
            for (uint32_t si = 0; si < skin->submeshCount; ++si)
            {
                skin->submeshes[si].boneCount      = static_cast<uint16_t>(windows[si].size());
                skin->submeshes[si].boneComboIndex = comboIndex[si];
                if (skin->submeshes[si].boneCount < 1) skin->submeshes[si].boneCount = 1;
                if (skin->submeshes[si].boneCount > widest) widest = skin->submeshes[si].boneCount;
            }
            // The widest window any one draw will bind. Modern skins leave this at zero because the
            // runtime they were authored for never asks; this one sizes the per-draw palette from it,
            // and zero sizes it to nothing.
            skin->boneCountMax = widest;
            skin->bones = bones;

            md->boneCombos.count  = static_cast<uint32_t>(slotCombos.size());
            md->boneCombos.offset = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(combos));

            PaletteRestore keep;
            keep.boneMap    = std::move(boneMap);
            keep.comboIndex = std::move(comboIndex);
            g_paletteMaps[md] = std::move(keep);

            splitMap.assign(skin->submeshCount, SplitRun{ 0, 1 });
            for (uint16_t i = 0; i < skin->submeshCount; ++i) splitMap[i].first = i;
            return true;
        }
    }

    bool CaptureVertexOrigin(const fmt::M2Header* header, const uint8_t* body, uint32_t size) noexcept
    {
        if (!header) return false;
        try
        {
            const std::lock_guard<std::mutex> lock(g_vertexOriginMutex);
            g_vertexOrigins.erase(header);
            wxl_modern_m2::vertexorigin::Origin value{};
            if (!wxl_modern_m2::vertexorigin::Read(body, size, header->vertices.offset,
                                                 header->vertices.count, value)) return false;
            g_vertexOrigins.emplace(header, value);
            return true;
        }
        catch (...) { return false; }
    }

    bool UsesExtendedIndexStart(std::string_view name)
    {
        return text::StartsWithCI(name, "character\\") ||
               text::StartsWithCI(name, "hd\\character\\") ||
               text::StartsWithCI(name, "item\\objectcomponents\\");
    }

    uint32_t FullIndexStart(const fmt::M2SkinSection& section, bool extended)
    {
        return extended ? ((static_cast<uint32_t>(section.level) << 16) | section.indexStart)
                        : static_cast<uint32_t>(section.indexStart);
    }

    /**
     * @brief Partitions over-ceiling submeshes into sub-sections, rebuilding the live skin geometry and
     *        header.boneCombos.
     * @param md          Parsed model header (boneCombos array is a raw pointer here).
     * @param skin        Live skin profile whose geometry arrays are rebuilt on success.
     * @param outSections Receives the rebuilt sub-sections.
     * @param splitMap    Receives the per-original-submesh sub-section run, indexed by original submesh.
     * @param splitCount  Receives the count of extra sub-draws produced.
     * @param name        Model path, used for logging.
     * @return true on commit; false (no commit) on any overflow, allocation failure, or missing array.
     */
    bool SplitSubmeshes(fmt::M2Header* md, Skin* skin, std::vector<SplitSection>& outSections, std::vector<SplitRun>& splitMap, uint32_t& splitCount, const char* name)
    {
        if (!md->boneCombos.count || !md->boneCombos.offset)
        {
            WLOG_WARN("modern-assets: '%s' has no bone combo table (count=%u); bone split impossible",
                      name, md->boneCombos.count);
            return false;
        }
        if (!skin->vertexLookup || !skin->indices || !skin->bones)
        {
            WLOG_WARN("modern-assets: '%s' skin is missing a geometry array (lookup=%d indices=%d"
                      " bones=%d); bone split impossible", name, skin->vertexLookup != nullptr,
                      skin->indices != nullptr, skin->bones != nullptr);
            return false;
        }

        // The compact and geometry paths both index four property bytes per skin
        // vertex. Some native skins advertise fewer property records. Do not read
        // their tail or invent bone slots; leave that contract to the native path.
        if (skin->boneCount < skin->vertexCount)
        {
            WLOG_WARN("modern-assets: '%s' bone properties=%u shorter than skin vertices=%u; skipping bone rewrite",
                      name ? name : "", skin->boneCount, skin->vertexCount);
            return false;
        }

        StabilizeCharacterVertices(md, skin,
                                   name ? std::string_view{ name } : std::string_view{});

        const SplitPolicy policy = PolicyFor(name ? std::string_view{ name } : std::string_view{});

        uint32_t boneComboCount = md->boneCombos.count;
        auto* boneCombos = reinterpret_cast<uint16_t*>(static_cast<uintptr_t>(md->boneCombos.offset));
        if (boneComboCount > kSplitMaxBoneCombos || !boneCombos)
        {
            WLOG_WARN("modern-assets: '%s' boneCombos count=%u out of range, skipping bone split", name, boneComboCount);
            return false;
        }

        // Renumbering alone carries almost every model that reaches here, and it can be done without
        // moving a single vertex or index. Only when a section genuinely references more bones than
        // one draw can bind is there anything left for the geometry split to do.
        if (!policy.forceGeometry &&
            CompactBoneWindows(md, skin, boneCombos, boneComboCount,
                               policy.maxBonesPerDraw, splitMap, name))
        {
            splitCount = 0;
            return true;
        }

        if (policy.forceGeometry)
            WLOG_INFO("orc-female-bone-split: forcing geometry path cap=%u sourceSections=%u"
                      " sourceVertices=%u sourceIndices=%u", policy.maxBonesPerDraw,
                      skin->submeshCount, skin->vertexCount, skin->indexCount);

        bool needsSplit = policy.forceGeometry;
        uint16_t worstBoneCount = 0;
        const bool extendedIndexStart = UsesExtendedIndexStart(name);
        for (uint32_t si = 0; si < skin->submeshCount; ++si)
        {
            const fmt::M2SkinSection& s = skin->submeshes[si];
            if (s.boneCount > worstBoneCount) worstBoneCount = s.boneCount;
            if ((extendedIndexStart || s.level == 0) && s.boneCount > policy.maxBonesPerDraw)
                needsSplit = true;
        }
        // Nothing over the ceiling is the ordinary answer and says nothing. Something over it that
        // still does not qualify is not: it means the submesh was passed over by the test above, and
        // the model will be handed to the engine asking for more bones per draw than it can bind.
        if (!needsSplit)
        {
            if (worstBoneCount > policy.maxBonesPerDraw)
                WLOG_WARN("modern-assets: '%s' wants %u bones in one draw (ceiling %u) but no submesh"
                          " qualified for splitting (extendedIndexStart=%d)",
                          name, worstBoneCount, policy.maxBonesPerDraw, extendedIndexStart);
            return false;
        }

        std::vector<uint16_t> newVtxLookup;
        std::vector<uint8_t>  newBones;
        std::vector<uint16_t> newIndices;
        // The two readings of the same table (see RestorePaletteMap): what the vertex fill must find
        // there, and what the palette upload must find there afterwards. They are built together so a
        // slice can never exist in one and not the other.
        std::vector<uint16_t> slotCombos(boneCombos, boneCombos + boneComboCount);
        std::vector<uint16_t> boneMap(boneCombos, boneCombos + boneComboCount);
        newVtxLookup.reserve(skin->vertexCount);
        newBones.reserve(skin->vertexCount * 4);
        newIndices.reserve(skin->indexCount);

        splitCount = 0;

        for (uint32_t si = 0; si < skin->submeshCount; ++si)
        {
            fmt::M2SkinSection s = skin->submeshes[si];

            // A level>0 submesh is a sub-batch the engine cannot draw. Pass it through as a single zeroed
            // placeholder so the batch re-point stays 1:1; its batch is later skipped.
            if (s.level > 0 && !extendedIndexStart)
            {
                s.level = 0; s.vertexStart = 0; s.vertexCount = 0; s.indexStart = 0;
                s.indexCount = 0; s.boneComboIndex = 0; s.centerBoneIndex = 0; s.boneCount = 1;
                reinterpret_cast<uint8_t*>(&s)[0x11] = 0;
                outSections.push_back({ s, static_cast<uint16_t>(si) });
                continue;
            }

            const uint32_t sourceIndexStart = FullIndexStart(s, extendedIndexStart);
            if (sourceIndexStart > skin->indexCount || s.indexCount > skin->indexCount - sourceIndexStart)
            {
                WLOG_WARN("modern-assets: '%s' submesh %u index window past skin indexCount, skipping bone split", name, si);
                return false;
            }

            uint32_t triCount = s.indexCount / 3;
            uint32_t comboBase = s.boneComboIndex;

            std::vector<uint16_t> curGlobals;
            uint32_t curTriStart = 0;
            uint32_t emittedSections = 0;

            // Every refusal below abandons the whole split, which leaves a model whose submeshes are
            // over the per-draw ceiling exactly as it was: undrawable, and silent about it. Naming the
            // limit that was hit is the difference between "it does not appear" and a fixable fact.
            auto refuse = [name, si](const char* why, uint32_t value) -> bool
            {
                WLOG_WARN("modern-assets: '%s' submesh %u: %s (%u), bone split abandoned",
                          name, si, why, value);
                return false;
            };

            // Emit triangles [triFrom, triTo) as one sub-section: a sorted boneCombos slice, a deduplicated
            // vertex block with bones[] remapped to the slice, and a global-indexed triangle block. The
            // slice is appended to both tables at once: the same position has to read back as the slot
            // while the vertices are numbered and as the bone once the palette is uploaded.
            auto emit = [&](uint32_t triFrom, uint32_t triTo, std::vector<uint16_t>& globals) -> bool
            {
                if (triFrom >= triTo) return true;
                std::sort(globals.begin(), globals.end());
                uint32_t comboIndex = static_cast<uint32_t>(boneMap.size());
                if (comboIndex > kSkinU16Max) return refuse("bone combo table past 16 bits", comboIndex);
                for (uint16_t slot = 0; slot < globals.size(); ++slot)
                {
                    boneMap.push_back(globals[slot]);
                    slotCombos.push_back(slot);
                }

                uint32_t secVertStart  = static_cast<uint32_t>(newVtxLookup.size());
                uint32_t secIndexStart = static_cast<uint32_t>(newIndices.size());
                if (secVertStart > kSkinU16Max) return refuse("rebuilt vertex block past 16 bits", secVertStart);
                if (!extendedIndexStart && secIndexStart > kSkinU16Max)
                    return refuse("rebuilt index block past 16 bits", secIndexStart);
                if (extendedIndexStart && (secIndexStart >> 16) > kSkinU16Max)
                    return refuse("rebuilt index block past the extended encoding", secIndexStart);

                std::unordered_map<uint16_t, uint16_t> vmap;
                for (uint32_t t = triFrom; t < triTo; ++t)
                {
                    for (uint32_t k = 0; k < 3; ++k)
                    {
                        uint16_t lv = skin->indices[sourceIndexStart + t * 3 + k];
                        if (lv >= skin->vertexCount) return refuse("index past the vertex list", lv);
                        auto it = vmap.find(lv);
                        uint16_t nv;
                        if (it == vmap.end())
                        {
                            uint32_t idx = static_cast<uint32_t>(newVtxLookup.size());
                            if (idx > kSkinU16Max) return refuse("rebuilt vertex list past 16 bits", idx);
                            nv = static_cast<uint16_t>(idx);
                            vmap.emplace(lv, nv);
                            newVtxLookup.push_back(skin->vertexLookup[lv]);
                            const uint8_t* infl = skin->bones + lv * 4;
                            for (uint32_t j = 0; j < 4; ++j)
                            {
                                uint32_t comboIdx = comboBase + infl[j];
                                uint16_t g = comboIdx < boneComboCount ? boneCombos[comboIdx] : globals[0];
                                auto lo = std::lower_bound(globals.begin(), globals.end(), g);
                                uint16_t local = (lo != globals.end() && *lo == g)
                                               ? static_cast<uint16_t>(lo - globals.begin()) : 0;
                                newBones.push_back(static_cast<uint8_t>(local));
                            }
                        }
                        else nv = it->second;
                        newIndices.push_back(nv);
                    }
                }

                uint32_t secVertCount  = static_cast<uint32_t>(newVtxLookup.size()) - secVertStart;
                uint32_t secIndexCount = static_cast<uint32_t>(newIndices.size()) - secIndexStart;
                if (secVertCount > kSkinU16Max) return refuse("sub-section vertices past 16 bits", secVertCount);
                if (secIndexCount > kSkinU16Max) return refuse("sub-section indices past 16 bits", secIndexCount);

                fmt::M2SkinSection sec = s;
                sec.vertexStart    = static_cast<uint16_t>(secVertStart);
                sec.vertexCount    = static_cast<uint16_t>(secVertCount);
                sec.level          = extendedIndexStart ? static_cast<uint16_t>(secIndexStart >> 16) : 0;
                sec.indexStart     = static_cast<uint16_t>(secIndexStart & kSkinU16Max);
                sec.indexCount     = static_cast<uint16_t>(secIndexCount);
                sec.boneCount      = static_cast<uint16_t>(globals.size());
                sec.boneComboIndex = static_cast<uint16_t>(comboIndex);
                outSections.push_back({ sec, static_cast<uint16_t>(si) });
                ++emittedSections;
                return true;
            };

            for (uint32_t t = 0; t < triCount; ++t)
            {
                uint16_t g[12]; int gn = 0;
                for (uint32_t k = 0; k < 3; ++k)
                {
                    uint16_t lv = skin->indices[sourceIndexStart + t * 3 + k];
                    if (lv >= skin->vertexCount) return refuse("index past the vertex list", lv);
                    const uint8_t* infl = skin->bones + lv * 4;
                    for (uint32_t j = 0; j < 4; ++j)
                    {
                        uint32_t comboIdx = comboBase + infl[j];
                        uint16_t gg = comboIdx < boneComboCount ? boneCombos[comboIdx] : 0;
                        bool seen = false;
                        for (int e = 0; e < gn; ++e) if (g[e] == gg) { seen = true; break; }
                        if (!seen && gn < 12) g[gn++] = gg;
                    }
                }
                size_t unionSize = curGlobals.size();
                for (int e = 0; e < gn; ++e)
                    if (std::find(curGlobals.begin(), curGlobals.end(), g[e]) == curGlobals.end())
                        ++unionSize;

                if (unionSize > policy.maxBonesPerDraw && t > curTriStart)
                {
                    if (!emit(curTriStart, t, curGlobals)) return false;
                    curGlobals.clear();
                    curTriStart = t;
                }
                for (int e = 0; e < gn; ++e)
                    if (std::find(curGlobals.begin(), curGlobals.end(), g[e]) == curGlobals.end())
                        curGlobals.push_back(g[e]);
            }
            if (!emit(curTriStart, triCount, curGlobals)) return false;
            if (emittedSections == 0)
            {
                fmt::M2SkinSection sec = s;
                sec.vertexCount = 0; sec.indexCount = 0; sec.boneCount = 1;
                outSections.push_back({ sec, static_cast<uint16_t>(si) });
            }
            else if (emittedSections > 1)
            {
                splitCount += emittedSections - 1;
            }
        }

        // The whole rebuild has to stay addressable by the client's own 16-bit skin lookup, and its
        // sub-draws have to stay within the batch ceiling. Either one exceeded means this model cannot
        // be expressed as stock skin geometry at all, which is worth saying out loud: it is a property
        // of the asset, not a transient failure.
        if (newVtxLookup.size() > kSkinU16Max)
        {
            WLOG_WARN("modern-assets: '%s' rebuilt to %u vertices, past the 16-bit skin lookup;"
                      " bone split abandoned", name, static_cast<uint32_t>(newVtxLookup.size()));
            return false;
        }
        if (outSections.size() > kMaxBatches)
        {
            WLOG_WARN("modern-assets: '%s' rebuilt to %u sub-draws, past the batch ceiling;"
                      " bone split abandoned", name, static_cast<uint32_t>(outSections.size()));
            return false;
        }

        splitMap.assign(skin->submeshCount, SplitRun{ 0, 0 });
        for (uint16_t i = 0; i < outSections.size(); ++i)
        {
            uint16_t orig = outSections[i].origSubmesh;
            if (orig >= splitMap.size()) continue;
            if (splitMap[orig].count == 0) splitMap[orig].first = i;
            ++splitMap[orig].count;
        }

        // Commit the rebuilt geometry into owned buffers (leaked for the model's lifetime; the engine never
        // per-array frees the file-mapped skin arrays).
        auto* vl = static_cast<uint16_t*>(std::malloc(newVtxLookup.size() * sizeof(uint16_t)));
        auto* bn = static_cast<uint8_t*>(std::malloc(newBones.size()));
        auto* ix = static_cast<uint16_t*>(std::malloc(newIndices.size() * sizeof(uint16_t)));
        auto* bc = static_cast<uint16_t*>(std::malloc(slotCombos.size() * sizeof(uint16_t)));
        auto* sm = static_cast<fmt::M2SkinSection*>(std::malloc(outSections.size() * sizeof(fmt::M2SkinSection)));
        // One vertex record per rebuilt skin vertex, carrying the register slot instead of the bone.
        // Owning the records lets two rebuilt sections assign different local slots to the same source
        // vertex while keeping the raw records and live skin-property array on the same contract.
        auto* srcVerts = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(md->vertices.offset));
        auto* vx = srcVerts && md->vertices.count
                 ? static_cast<uint8_t*>(std::malloc(newVtxLookup.size() * kVertexStride)) : nullptr;
        if (!vl || !bn || !ix || !bc || !sm || !vx)
        {
            std::free(vl); std::free(bn); std::free(ix); std::free(bc); std::free(sm); std::free(vx);
            WLOG_WARN("modern-assets: '%s' could not allocate the rebuilt geometry; bone split"
                      " abandoned", name);
            return false;
        }
        // Gather the records the rebuilt vertices point at, stamp the slots in, and leave the lookup as
        // the identity: after this each rebuilt vertex owns its record outright, which is what lets two
        // sections give the same source vertex two different slots.
        for (size_t i = 0; i < newVtxLookup.size(); ++i)
        {
            uint8_t* dst = vx + i * kVertexStride;
            const uint32_t source = newVtxLookup[i];
            // The walk proved the lookup ARRAY fits the body; nothing has yet proven what it holds,
            // and here its contents are a copy offset. A file naming a vertex it does not have loses
            // its rebuild rather than reading whatever follows the vertex block.
            if (source >= md->vertices.count)
            {
                std::free(vl); std::free(bn); std::free(ix); std::free(bc); std::free(sm); std::free(vx);
                WLOG_WARN("modern-assets: '%s' vertex lookup names vertex %u of %u; bone split"
                          " abandoned", name, source, md->vertices.count);
                return false;
            }
            std::memcpy(dst, srcVerts + static_cast<size_t>(source) * kVertexStride, kVertexStride);
            for (uint32_t k = 0; k < 4; ++k)
                dst[kOffVertexBoneIdx + k] = newBones[i * 4 + k];
            newVtxLookup[i] = static_cast<uint16_t>(i);
        }

        std::memcpy(vl, newVtxLookup.data(), newVtxLookup.size() * sizeof(uint16_t));
        std::memcpy(bn, newBones.data(), newBones.size());
        std::memcpy(ix, newIndices.data(), newIndices.size() * sizeof(uint16_t));
        std::memcpy(bc, slotCombos.data(), slotCombos.size() * sizeof(uint16_t));
        for (size_t i = 0; i < outSections.size(); ++i) sm[i] = outSections[i].section;

        skin->vertexLookup = vl;
        skin->vertexCount  = static_cast<uint32_t>(newVtxLookup.size());
        skin->bones        = bn;
        skin->boneCount    = static_cast<uint32_t>(newVtxLookup.size());
        skin->indices      = ix;
        skin->indexCount   = static_cast<uint32_t>(newIndices.size());
        skin->submeshes    = sm;
        skin->submeshCount = static_cast<uint32_t>(outSections.size());

        // Same reason as the renumbering path: the widest window a single draw will bind is a property
        // of the geometry that was just rebuilt, and nothing else states it.
        uint32_t widest = 1;
        for (const SplitSection& section : outSections)
            if (section.section.boneCount > widest) widest = section.section.boneCount;
        skin->boneCountMax = widest;

        // The slot-numbered table goes live now, because the vertex fill inside the native finalize is
        // the next thing to read it. The bone-numbered one is held for RestorePaletteMap to put back
        // once that fill has happened and the finalize has flattened this buffer to the identity.
        md->vertices.count  = static_cast<uint32_t>(newVtxLookup.size());
        md->vertices.offset = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(vx));

        md->boneCombos.count  = static_cast<uint32_t>(slotCombos.size());
        md->boneCombos.offset = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(bc));

        PaletteRestore keep;
        keep.boneMap = std::move(boneMap);
        keep.comboIndex.reserve(outSections.size());
        for (const SplitSection& section : outSections)
            keep.comboIndex.push_back(section.section.boneComboIndex);
        g_paletteMaps[md] = std::move(keep);
        if (policy.forceGeometry)
            WLOG_INFO("orc-female-bone-split: rebuilt cap=%u sections=%u extra=%u vertices=%u"
                      " indices=%u combos=%u boneCountMax=%u", policy.maxBonesPerDraw,
                      skin->submeshCount, splitCount, skin->vertexCount, skin->indexCount,
                      md->boneCombos.count, skin->boneCountMax);
        return true;
    }

    /**
     * @brief Republishes the palette window every section owns, over what the finalize left behind.
     * @param model Runtime model pointer.
     * @return true when a map was stored for this model's header and has been republished.
     */
    bool RestorePaletteMap(void* model)
    {
        if (!model) return false;
        wxl::game::m2::M2Model wrapper(model);
        const char* const path = wrapper.GetPathStem();
        const bool trace = path && IsFemaleOrcCanary(path);
        uint32_t shaderFlag = 0;
        const bool shaderFlagReadable = trace && ReadShaderPathFlag(shaderFlag);
        auto* md = wrapper.GetHeader();
        auto* skin = wrapper.GetSkin();
        if (!md || !skin || !skin->submeshes)
        {
            if (trace)
                WLOG_WARN("orc-female-palette: restore=failed reason=missing-runtime shaderFlag=%#x"
                          " branch=%s header=%p skin=%p sections=%p path='%s'",
                          shaderFlag, ShaderPathName(shaderFlagReadable, shaderFlag), md, skin,
                          skin ? skin->submeshes : nullptr, path);
            return false;
        }

        const uint16_t preLiveBones = skin->submeshCount ? skin->submeshes[0].boneCount : 0;
        const uint16_t preLiveCombo = skin->submeshCount ? skin->submeshes[0].boneComboIndex : 0;

        auto it = g_paletteMaps.find(md);
        if (it == g_paletteMaps.end())
        {
            if (trace)
                WLOG_WARN("orc-female-palette: restore=failed reason=no-saved-map shaderFlag=%#x"
                          " branch=%s combos=%u sections=%u live0=%u@%u path='%s'",
                          shaderFlag, ShaderPathName(shaderFlagReadable, shaderFlag),
                          md->boneCombos.count, skin->submeshCount, preLiveBones, preLiveCombo, path);
            return false;
        }
        const PaletteRestore& keep = it->second;

        auto* combos = reinterpret_cast<uint16_t*>(static_cast<uintptr_t>(md->boneCombos.offset));
        // The rebuild sized these together, so counts that no longer match mean the model was
        // re-pointed at other geometry in between and this map no longer describes it.
        if (!combos || md->boneCombos.count != keep.boneMap.size() ||
            skin->submeshCount != keep.comboIndex.size())
        {
            if (trace)
                WLOG_WARN("orc-female-palette: restore=failed reason=contract-mismatch shaderFlag=%#x"
                          " branch=%s comboPtr=%p combos=%u/%u sections=%u/%u live0=%u@%u"
                          " path='%s'",
                          shaderFlag, ShaderPathName(shaderFlagReadable, shaderFlag), combos,
                          md->boneCombos.count, static_cast<uint32_t>(keep.boneMap.size()),
                          skin->submeshCount, static_cast<uint32_t>(keep.comboIndex.size()),
                          preLiveBones, preLiveCombo, path);
            return false;
        }

        // Do not touch the finalize-owned copy until the stored map has passed its contract checks.
        // On the successful path RestorePaletteMap already writes this buffer, so these two reads add
        // no new pointer reachability beyond the operation being diagnosed.
        auto* copy = *reinterpret_cast<fmt::M2SkinSection**>(
            static_cast<uint8_t*>(model) + off::kOffModelSubmeshBuf);
        const uint16_t preCopyBones = copy && skin->submeshCount ? copy[0].boneCount : 0;
        const uint16_t preCopyCombo = copy && skin->submeshCount ? copy[0].boneComboIndex : 0;

        uint16_t preHead[4]{};
        const uint32_t preHeadCount = std::min<uint32_t>(md->boneCombos.count, 4u);
        for (uint32_t i = 0; i < preHeadCount; ++i) preHead[i] = combos[i];

        std::memcpy(combos, keep.boneMap.data(), keep.boneMap.size() * sizeof(uint16_t));

        // The finalize keeps a second copy of the section table for its own draw bookkeeping and
        // zeroes the window start in both. Both are restored, because which of the two a given draw
        // path reads is not something this side gets to choose.
        for (uint32_t i = 0; i < skin->submeshCount; ++i)
        {
            skin->submeshes[i].boneComboIndex = keep.comboIndex[i];
            if (copy) copy[i].boneComboIndex = keep.comboIndex[i];
        }

        if (trace)
        {
            const uint16_t postLiveBones = skin->submeshCount ? skin->submeshes[0].boneCount : 0;
            const uint16_t postLiveCombo = skin->submeshCount ? skin->submeshes[0].boneComboIndex : 0;
            const uint16_t postCopyBones = copy && skin->submeshCount ? copy[0].boneCount : 0;
            const uint16_t postCopyCombo = copy && skin->submeshCount ? copy[0].boneComboIndex : 0;
            uint16_t globals[8]{};
            uint32_t shown = 0;
            if (skin->submeshCount && postLiveCombo < md->boneCombos.count)
            {
                shown = std::min<uint32_t>(postLiveBones, 8u);
                shown = std::min<uint32_t>(shown, md->boneCombos.count - postLiveCombo);
                for (uint32_t i = 0; i < shown; ++i) globals[i] = combos[postLiveCombo + i];
            }
            WLOG_INFO("orc-female-palette: restore=ok shaderFlag=%#x branch=%s combos=%u"
                      " sections=%u boneCountMax=%u preHead[%u]=%u,%u,%u,%u"
                      " live0=%u@%u->%u@%u copy=%p copy0=%u@%u->%u@%u"
                      " section0Globals[%u]=%u,%u,%u,%u,%u,%u,%u,%u path='%s'",
                      shaderFlag, ShaderPathName(shaderFlagReadable, shaderFlag), md->boneCombos.count,
                      skin->submeshCount, skin->boneCountMax, preHeadCount,
                      preHead[0], preHead[1], preHead[2], preHead[3],
                      preLiveBones, preLiveCombo, postLiveBones, postLiveCombo, copy,
                      preCopyBones, preCopyCombo, postCopyBones, postCopyCombo, shown,
                      globals[0], globals[1], globals[2], globals[3], globals[4], globals[5],
                      globals[6], globals[7], path);
        }
        return true;
    }

    /**
     * @brief Drops the stored palette map for a header whose model is being reused or torn down.
     * @param md Parsed model header.
     */
    void ForgetPaletteMap(const fmt::M2Header* md)
    {
        if (md)
        {
            g_paletteMaps.erase(md);
            {
                const std::lock_guard<std::mutex> lock(g_vertexOriginMutex);
                g_vertexOrigins.erase(md);
            }
            // The stable array follows the same model-lifetime allocation policy as the rebuilt
            // skin/header arrays: forget the reuse guard, but do not free storage the native model
            // may still reference while its teardown is in flight.
            g_stableCharacterVertices.erase(md);
        }
    }

    /**
     * @brief Re-points a skin's existing batches across the sub-section run their original submesh
     *        became, duplicating a batch per extra sub-section without touching any other field.
     * @param skin      Live skin profile whose batches are rebuilt in place.
     * @param splitMap  Per-original-submesh sub-section run, as produced by SplitSubmeshes.
     */
    void RepointBatchesAfterSplit(Skin* skin, const std::vector<SplitRun>& splitMap)
    {
        if (splitMap.empty() || !skin->batches || skin->batchCount == 0) return;

        std::vector<fmt::M2Batch> out;
        out.reserve(skin->batchCount);
        for (uint32_t i = 0; i < skin->batchCount; ++i)
        {
            const fmt::M2Batch& b = skin->batches[i];
            SplitRun run{ b.skinSectionIndex, 1 };
            if (b.skinSectionIndex < splitMap.size()) run = splitMap[b.skinSectionIndex];
            for (uint16_t s = 0; s < run.count; ++s)
            {
                fmt::M2Batch nb = b;
                nb.skinSectionIndex = static_cast<uint16_t>(run.first + s);
                out.push_back(nb);
            }
        }
        if (out.empty() || out.size() > kMaxBatches) return;

        // Leaked for the model's lifetime, same pattern as SplitSubmeshes' committed arrays above.
        auto* buf = static_cast<fmt::M2Batch*>(std::malloc(out.size() * sizeof(fmt::M2Batch)));
        if (!buf) return;
        std::memcpy(buf, out.data(), out.size() * sizeof(fmt::M2Batch));
        skin->batches    = buf;
        skin->batchCount = static_cast<uint32_t>(out.size());
    }
}
