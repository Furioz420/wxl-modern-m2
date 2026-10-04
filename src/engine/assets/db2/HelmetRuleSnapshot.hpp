#pragma once
#include "ItemDisplayIndex.hpp"
#include "wxl/RetailDb2Api.h"

namespace wxl::runtime::db2::itemdisplay
{
    // Visibility IDs can come from native DBC rows which never enter the Retail
    // display graph. Query the complete helmet table by ID, independently.
    inline std::shared_ptr<const std::vector<HelmetGeosetRule>> ReadHelmetRules(
        const WXL_RetailDb2Api& api, void* lease, uint32_t visibilityId)
    {
        if (!lease || !visibilityId || !api.IndexHelmetRuleCount || !api.IndexHelmetRuleAt)
            return {};
        auto rules = std::make_shared<std::vector<HelmetGeosetRule>>();
        const auto count = api.IndexHelmetRuleCount(lease, visibilityId);
        rules->reserve(count);
        for (uint32_t i = 0; i < count; ++i)
        {
            WXL_RetailHelmetGeosetRule raw{};
            if (!api.IndexHelmetRuleAt(lease, visibilityId, i, &raw)) return {};
            rules->push_back({raw.raceId, raw.hideGroup, raw.raceBitSelection, raw.flags});
        }
        return rules;
    }
}
