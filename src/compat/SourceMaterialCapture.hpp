// Copyright (C) 2026 WarcraftXL. GPL-3.0-or-later.
// Owned source metadata only. No renderer decisions or borrowed asset-array pointers.
#pragma once

#include "SourceMaterial.hpp"
#include "SourceParticle.hpp"
#include "SourceRibbon.hpp"
#include "engine/assets/shared/models/m2/M2Format.hpp"

#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace wxl::modern::assets::m2::material
{
    struct SourceRenderFlags { uint16_t flags, blend; };
    static_assert(sizeof(SourceRenderFlags) == 4);

    struct ModelSource
    {
        // MD21 + inner version is not sufficient evidence for a particular effect table.
        Profile profile = Profile::Unknown;
        uint32_t containerMagic = wxl::structure::m2::kMagicMD21;
        uint32_t innerVersion = 0;
        uint32_t globalFlags = 0;
        uint32_t skippedChunks = UINT32_MAX; // unknown unless supplied by the bounded container scanner
        std::vector<SourceRenderFlags> materials;
        // filename offsets below are ORIGINAL file offsets, never live pointers.
        std::vector<wxl::structure::m2::M2Texture> textures;
        std::vector<uint32_t> textureFileDataIds;
        std::vector<uint16_t> textureCombos, coordCombos, weightCombos, transformCombos;
        std::vector<uint16_t> combinerCombos;
        ParticleCapture particleCapture = ParticleCapture::Absent;
        uint32_t sourceParticleCount = 0;
        uint32_t sourceRibbonCount = 0, sourceTransformCount = 0;
        bool ribbonsCaptured = false;
        std::vector<SourceRibbon> ribbons;
        std::vector<SourceParticle> particles;
        bool particleLayerFeaturesKnown = false;
        uint32_t riftNativeSpriteMask = 0; // exact-source approximation, never features-known
        std::vector<std::array<float,2>> particleMultipliers; // EXP2 RGB/alpha; owned, validated subset
        size_t StorageBytes() const noexcept;
    };

    struct BatchOrigin
    {
        uint32_t sourceBatch;
        uint16_t piece;        // translator pass ordinal, not a new source material
        uint16_t split;        // bone-split ordinal within the source section
        uint16_t section;      // final native section
        bool parked;
    };

    struct SkinSource
    {
        const void* owner = nullptr; // allocation identity; never dereferenced by the store
        const void* skin = nullptr;
        const void* installedBatches = nullptr;
        std::shared_ptr<const ModelSource> model;
        std::vector<wxl::structure::m2::M2Batch> batches; // exact pre-translation records
        std::vector<BatchOrigin> outputs;               // final batch index -> source
        bool valid = true;
        size_t StorageBytes() const noexcept;
        Classification ClassifySource(uint32_t index) const noexcept;
    };

    // All operations are best-effort and cannot propagate C++ allocation exceptions into
    // the native loader. Missing capture must never change existing rendering behavior.
    // Limits bound retained vector capacity; allocator/map overhead is additionally bounded
    // by maxModels and maxSkinsPerModel. Consumers can extend lifetime by retaining Find().
    class SourceMaterialStore
    {
    public:
        struct Limits
        {
            size_t bytes = 16u * 1024u * 1024u;
            size_t maxModels = 1024;
            size_t maxSkinsPerModel = 8;
        };
        SourceMaterialStore() = default;
        explicit SourceMaterialStore(Limits limits) : limits_(limits) {}

        bool CaptureRaw(const void* owner, const uint8_t* base, uint32_t size,
                        const uint32_t* txids, uint32_t txidCount, uint32_t skippedChunks = UINT32_MAX,
                        const uint8_t* container = nullptr, uint32_t containerSize = 0) noexcept;
        // Particle-only M2s have no SKIN batches. Retrieval must not depend on Begin/Commit.
        std::shared_ptr<const ModelSource> FindModel(const void* owner) const noexcept;
        std::unique_ptr<SkinSource> Begin(const void* owner, const void* skin,
            const wxl::structure::m2::M2Batch* batches, uint32_t count) noexcept;
        static void Append(SkinSource* draft, uint32_t sourceBatch, uint16_t piece,
                           uint16_t split, uint16_t section, bool parked) noexcept;
        bool Commit(std::unique_ptr<SkinSource> draft, const void* installedBatches,
                    uint32_t installedCount) noexcept;
        std::shared_ptr<const SkinSource> Find(const void* owner, const void* skin,
                    const void* installedBatches, uint32_t installedCount) const noexcept;
        void Forget(const void* owner) noexcept;
        size_t RetainedBytes() const noexcept;

    private:
        struct Entry
        {
            struct Slot
            {
                const void* skin = nullptr;
                std::shared_ptr<const SkinSource> snapshot;
            };
            std::shared_ptr<const ModelSource> model;
            // Fixed slots let Begin reserve a no-allocation tombstone before copying.
            std::array<Slot, 8> skins{};
            size_t used = 0;
        };
        void EraseLocked(const void* owner);
        Limits limits_;
        mutable std::mutex mutex_;
        std::unordered_map<const void*, Entry> entries_;
        size_t bytes_ = 0;
    };

    SourceMaterialStore& SourceMaterials();
}
