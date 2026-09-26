#pragma once
#include <cstdint>
namespace wxl_modern_m2::dragon {
// Choice IDs are globally unique. Keep old saved/catalog rows valid while
// rendering their hornless choice as Swept; new selections save Swept itself.
inline constexpr uint32_t RequiredHorns(uint32_t choice)
{
    return choice == 19507u ? 26861u : choice;
}
}
