// Local immutable facade over wxl-db2's ABI-safe retail catalog lease.
// Copyright (C) 2026 WarcraftXL. GPLv3.

#include "RetailItemCatalog.hpp"

#include "ExtensionApi.hpp"

#include <mutex>

namespace wxl::runtime::db2::retailitems
{
    namespace
    {
        std::mutex g_mutex;
        std::shared_ptr<const Catalog> g_catalog;

        std::shared_ptr<const Catalog> Import()
        {
            const WXL_RetailDb2Api* api = wxl_modern_m2::RetailDb2();
            if (!api || !api->Enabled || !api->Enabled()) return {};
            void* lease = api->AcquireCatalog ? api->AcquireCatalog() : nullptr;
            if (!lease) return {};

            struct Release
            {
                const WXL_RetailDb2Api* api;
                void* lease;
                ~Release() { api->ReleaseCatalog(lease); }
            } release{api, lease};

            auto catalog = std::make_shared<Catalog>();
            const uint32_t itemCount = api->CatalogItemCount(lease);
            catalog->items.reserve(itemCount);
            for (uint32_t i = 0; i < itemCount; ++i)
            {
                uint32_t id = 0;
                WXL_RetailItemInfo value{};
                if (!api->CatalogItemAt(lease, i, &id, &value)) continue;
                catalog->items.emplace(id, Item{
                    value.classId, value.subclassId, value.soundOverride,
                    value.itemGroupSoundsId, value.material, value.inventoryType,
                    value.sheatheType, value.displayId, value.appearanceId,
                    value.iconFileDataId,
                });
            }

            const uint32_t variantCount = api->CatalogVariantCount(lease);
            catalog->variants.reserve(variantCount);
            for (uint32_t i = 0; i < variantCount; ++i)
            {
                uint64_t key = 0;
                WXL_RetailItemVariant value{};
                if (api->CatalogVariantAt(lease, i, &key, &value))
                    catalog->variants.emplace(key, Catalog::Variant{
                        value.displayId, value.appearanceId, value.iconFileDataId,
                    });
            }

            const uint32_t iconCount = api->CatalogIconCount(lease);
            catalog->iconByDisplay.reserve(iconCount);
            for (uint32_t i = 0; i < iconCount; ++i)
            {
                uint32_t display = 0, file = 0;
                if (api->CatalogIconAt(lease, i, &display, &file))
                    catalog->iconByDisplay.emplace(display, file);
            }

            const uint32_t soundCount = api->CatalogSoundCount(lease);
            catalog->soundByDisplay.reserve(soundCount);
            for (uint32_t i = 0; i < soundCount; ++i)
            {
                uint32_t display = 0, sound = 0;
                if (api->CatalogSoundAt(lease, i, &display, &sound))
                    catalog->soundByDisplay.emplace(display, sound);
            }

            const uint32_t displayCount = api->CatalogDisplayCount(lease);
            catalog->displayIds.reserve(displayCount);
            for (uint32_t i = 0; i < displayCount; ++i)
            {
                uint32_t display = 0;
                if (api->CatalogDisplayAt(lease, i, &display))
                    catalog->displayIds.insert(display);
            }
            return catalog;
        }
    }

    void Publish(std::shared_ptr<Catalog> catalog)
    {
        const std::lock_guard lock(g_mutex);
        g_catalog = std::move(catalog);
    }

    std::shared_ptr<const Catalog> Current()
    {
        const std::lock_guard lock(g_mutex);
        if (!g_catalog) g_catalog = Import();
        return g_catalog;
    }
}
