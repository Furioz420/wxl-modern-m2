// UI identity is global; world appearance and attachment recipes are not.
#pragma once
#include <cstdint>
namespace wxl_modern_m2::customization
{
    inline bool IsPreviewMap(int32_t mapId) { return mapId < 0; }
    inline bool SameRecipe(const void* rootA, uint32_t modelA, uint32_t recipeA,
                           const void* rootB, uint32_t modelB, uint32_t recipeB)
    {
        return rootA == rootB && modelA == modelB && recipeA == recipeB;
    }
    inline bool CurrentRoot(const void* requested, const void* live)
    {
        return requested == live;
    }
}
