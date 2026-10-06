// Copyright (C) 2026 WarcraftXL. GPL-3.0-or-later.
#include "SourceMaterialCapture.hpp"
#include "ParticleLayerFeatures.hpp"
#include "RiftParticleContract.hpp"

#include <cstddef>
#include <cstring>

namespace wxl::modern::assets::m2::material
{
    namespace fmt = wxl::structure::m2;
    namespace
    {
        constexpr uint32_t kMaxArray = 0x10000;
        constexpr uint32_t kMaxSourceBatches = 0x4000;
        constexpr uint32_t kMaxOutputBatches = 0x10000;

        template<class T>
        size_t Bytes(const std::vector<T>& values) noexcept
        {
            return values.capacity() * sizeof(T);
        }

        template<class T>
        bool CopyRaw(const uint8_t* base, uint32_t size, fmt::M2Array array,
                     std::vector<T>& output)
        {
            if (!array.count) return true;
            // Divide the remaining range instead of multiplying untrusted counts.
            if (!array.offset || array.count > kMaxArray || array.offset > size ||
                array.count > (size - array.offset) / sizeof(T)) return false;
            output.resize(array.count);
            std::memcpy(output.data(), base + array.offset, array.count * sizeof(T));
            return true;
        }
    }

    size_t ModelSource::StorageBytes() const noexcept
    {
        return sizeof(*this) + Bytes(materials) + Bytes(meshTxac) + Bytes(textures) + Bytes(textureFileDataIds) +
            Bytes(textureCombos) + Bytes(coordCombos) + Bytes(weightCombos) +
            Bytes(transformCombos) + Bytes(combinerCombos) + Bytes(particles) + Bytes(particleMultipliers) + Bytes(nativeSpriteLayers) + Bytes(spriteLayerKinds) + Bytes(ribbons);
    }

    size_t SkinSource::StorageBytes() const noexcept
    {
        return sizeof(*this) + Bytes(batches) + Bytes(outputs);
    }

    Classification SkinSource::ClassifySource(uint32_t index) const noexcept
    {
        if (!model || index >= batches.size()) return Classify(Profile::Unknown, 0, 0);
        const auto& batch = batches[index];
        return Classify(model->profile, batch.shaderId, batch.textureCount);
    }

    void SourceMaterialStore::EraseLocked(const void* owner)
    {
        const auto it = entries_.find(owner);
        if (it == entries_.end()) return;
        bytes_ -= it->second.model->StorageBytes();
        for (const auto& skin : it->second.skins)
            if (skin.snapshot) bytes_ -= skin.snapshot->StorageBytes();
        entries_.erase(it);
    }

    bool SourceMaterialStore::CaptureRaw(const void* owner, const uint8_t* base,
        uint32_t size, const uint32_t* txids, uint32_t txidCount, uint32_t skippedChunks,
        const uint8_t* container, uint32_t containerSize) noexcept
    {
        try
        {
            const std::lock_guard lock(mutex_);
            EraseLocked(owner); // replacement/failure cannot leave the old generation visible
            if (!owner || !base || size < sizeof(fmt::M2Header) ||
                txidCount > kMaxArray || (txidCount && !txids) ||
                entries_.size() >= limits_.maxModels) return false;
            fmt::M2Header header;
            std::memcpy(&header, base, sizeof(header));
            if (header.magic != fmt::kMagicMD20) return false;
            auto source = std::make_shared<ModelSource>();
            source->innerVersion = header.version;
            source->globalFlags = header.globalFlags;
            source->skippedChunks = skippedChunks;
            if (!CopyRaw(base, size, header.materials, source->materials) ||
                !CopyRaw(base, size, header.textures, source->textures) ||
                !CopyRaw(base, size, header.textureCombos, source->textureCombos) ||
                !CopyRaw(base, size, header.textureUnitLookup, source->coordCombos) ||
                !CopyRaw(base, size, header.textureWeightCombos, source->weightCombos) ||
                !CopyRaw(base, size, header.textureTransformCombos, source->transformCombos) ||
                ((header.globalFlags & fmt::kFlagUseTextureCombinerCombos) &&
                 !CopyRaw(base, size, header.textureCombinerCombos, source->combinerCombos)))
                return false;
            if (txidCount) source->textureFileDataIds.assign(txids, txids + txidCount);
            try {
                source->meshTxacState=ReadMeshTxac(container,containerSize,
                    header.materials.count,header.particleEmitters.count,source->meshTxac);
            } catch(...) {
                std::vector<std::array<uint8_t,2>>().swap(source->meshTxac);
                source->meshTxacState=MeshTxacState::Unknown;
            }
            source->sourceRibbonCount=header.ribbonEmitters.count;
            source->sourceTransformCount=header.textureTransforms.count;
            if(header.version>=272 && header.version<=274 && header.ribbonEmitters.count<=128) {
                try {
                    std::vector<fmt::M2Ribbon> raw;
                    bool ok=CopyRaw(base,size,header.ribbonEmitters,raw);
                    if(ok && source->StorageBytes()<=limits_.bytes-bytes_ &&
                        raw.size()*sizeof(SourceRibbon)<=limits_.bytes-bytes_-source->StorageBytes()) {
                        source->ribbons.resize(raw.size());
                        for(size_t n=0;n<raw.size()&&ok;++n)ok=CaptureRibbon(base,size,raw[n],source->ribbons[n]);
                        source->ribbonsCaptured=ok;
                    }
                    if(!source->ribbonsCaptured)std::vector<SourceRibbon>().swap(source->ribbons);
                } catch(...) {std::vector<SourceRibbon>().swap(source->ribbons);source->ribbonsCaptured=false;}
            }
            source->sourceParticleCount = header.particleEmitters.count;
            if (header.particleEmitters.count)
            {
                const auto array = header.particleEmitters;
                if (header.version < 272 || header.version > 274)
                    source->particleCapture = ParticleCapture::UnsupportedVersion;
                else if (array.count > 2048)
                    source->particleCapture = ParticleCapture::OverBudget;
                else if (!array.offset || array.offset > size ||
                         array.count > (size - array.offset) / SourceParticle::kStride)
                    source->particleCapture = ParticleCapture::InvalidRange;
                else if (source->StorageBytes() > limits_.bytes - bytes_ ||
                         size_t(array.count) * sizeof(SourceParticle) >
                         limits_.bytes - bytes_ - source->StorageBytes())
                    source->particleCapture = ParticleCapture::OverBudget;
                else
                {
                    // This optional capture must not invalidate otherwise usable mesh metadata.
                    try
                    {
                        if (CopyRaw(base, size, array, source->particles))
                            source->particleCapture = ParticleCapture::Captured;
                    }
                    catch (...)
                    {
                        std::vector<SourceParticle>().swap(source->particles);
                        source->particleCapture = ParticleCapture::OverBudget;
                    }
                }
            }
            if (source->particleCapture == ParticleCapture::Captured)
                source->particleLayerFeaturesKnown = CaptureLayerFeatures(base,size,container,containerSize,*source);
            CaptureNativeSpriteLayers(base,size,container,containerSize,*source);
              source->riftNativeSpriteMask = CaptureRiftNativeSpriteContract(container,containerSize,*source);
            if (source->particleCapture == ParticleCapture::Captured && source->StorageBytes() > limits_.bytes - bytes_)
            {
                std::vector<SourceParticle>().swap(source->particles);
                std::vector<std::array<float,2>>().swap(source->particleMultipliers);
                source->particleLayerFeaturesKnown = false;
                  source->nativeSpriteLayers.clear();
                source->spriteLayerKinds.clear();
                source->riftNativeSpriteMask = 0;
                source->txac11SpriteMask = 0;
                source->particleCapture = ParticleCapture::OverBudget;
            }
            const size_t bytes = source->StorageBytes();
            if (bytes > limits_.bytes - bytes_) return false;
            entries_.emplace(owner, Entry{std::move(source), {}});
            bytes_ += bytes;
            return true;
        }
        catch (...) { return false; }
    }

    std::unique_ptr<SkinSource> SourceMaterialStore::Begin(const void* owner,
        const void* skin, const fmt::M2Batch* batches, uint32_t count) noexcept
    {
        try
        {
            const std::lock_guard lock(mutex_);
            const auto it = entries_.find(owner);
            if (it == entries_.end() || !skin) return nullptr;
            auto& entry = it->second;
            // Never relabel previously translated records as original source data.
            // A reused skin identity stays unavailable until a fresh model generation.
            // Keep its old token so repeated finalizes cannot bypass this guard.
            for (const auto& slot : entry.skins)
                if (slot.skin == skin) return nullptr;
            if (entry.used >= limits_.maxSkinsPerModel || entry.used >= entry.skins.size())
                return nullptr;
            // Reserve a tombstone BEFORE copying. Even an abandoned/failed commit cannot
            // cause a later finalize to capture compatibility output as original data.
            entry.skins[entry.used++].skin = skin;
            if (!batches || !count || count > kMaxSourceBatches) return nullptr;
            auto draft = std::make_unique<SkinSource>();
            draft->owner = owner;
            draft->skin = skin;
            draft->model = it->second.model;
            draft->batches.assign(batches, batches + count);
            return draft;
        }
        catch (...) { return nullptr; }
    }

    void SourceMaterialStore::Append(SkinSource* draft, uint32_t sourceBatch,
        uint16_t piece, uint16_t split, uint16_t section, bool parked) noexcept
    {
        if (!draft || !draft->valid) return;
        if (sourceBatch >= draft->batches.size() || draft->outputs.size() >= kMaxOutputBatches)
        {
            draft->valid = false;
            return;
        }
        try { draft->outputs.push_back({sourceBatch, piece, split, section, parked}); }
        catch (...) { draft->valid = false; }
    }

    bool SourceMaterialStore::Commit(std::unique_ptr<SkinSource> draft,
        const void* installedBatches, uint32_t installedCount) noexcept
    {
        if (!draft || !draft->valid || !installedBatches || !installedCount ||
            draft->outputs.size() != installedCount) return false;
        try
        {
            const std::lock_guard lock(mutex_);
            const auto it = entries_.find(draft->owner);
            if (it == entries_.end() || it->second.model != draft->model) return false;
            Entry::Slot* slot = nullptr;
            for (auto& candidate : it->second.skins)
                if (candidate.skin == draft->skin) { slot = &candidate; break; }
            if (!slot || slot->snapshot) return false;
            const size_t bytes = draft->StorageBytes();
            if (bytes > limits_.bytes - bytes_) return false;
            draft->installedBatches = installedBatches;
            std::shared_ptr<const SkinSource> frozen(std::move(draft));
            slot->snapshot = std::move(frozen);
            bytes_ += bytes;
            return true;
        }
        catch (...) { return false; }
    }

    std::shared_ptr<const SkinSource> SourceMaterialStore::Find(const void* owner,
        const void* skin, const void* installedBatches, uint32_t installedCount) const noexcept
    {
        try
        {
            const std::lock_guard lock(mutex_);
            const auto model = entries_.find(owner);
            if (model == entries_.end()) return nullptr;
            for (const auto& slot : model->second.skins)
                if (slot.skin == skin && slot.snapshot &&
                    slot.snapshot->installedBatches == installedBatches &&
                    slot.snapshot->outputs.size() == installedCount) return slot.snapshot;
            return nullptr;
        }
        catch (...) { return nullptr; }
    }

    std::shared_ptr<const ModelSource> SourceMaterialStore::FindModel(const void* owner) const noexcept
    {
        try
        {
            const std::lock_guard lock(mutex_);
            const auto found = entries_.find(owner);
            return found == entries_.end() ? nullptr : found->second.model;
        }
        catch (...) { return nullptr; }
    }

    void SourceMaterialStore::Forget(const void* owner) noexcept
    {
        try { const std::lock_guard lock(mutex_); EraseLocked(owner); }
        catch (...) {} // no exceptions may escape a native deallocator
    }

    size_t SourceMaterialStore::RetainedBytes() const noexcept
    {
        try { const std::lock_guard lock(mutex_); return bytes_; }
        catch (...) { return 0; }
    }

    SourceMaterialStore& SourceMaterials()
    {
        static SourceMaterialStore store;
        return store;
    }
}
