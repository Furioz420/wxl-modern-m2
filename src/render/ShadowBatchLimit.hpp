// Preserve the native shadow loop; limit only its per-iteration instance grant.
#pragma once
#include <algorithm>
#include <cstdint>

namespace wxl_modern_m2::shadowbatch
{
    struct Context { const void* model = nullptr; uint32_t limit = 0; };
    inline thread_local Context active;

    // Scoped to the exact shared model and rendering thread. Nested draws restore their caller.
    class Scope
    {
        Context previous_;
    public:
        Scope(const void* model, uint32_t limit) : previous_(active) { active = { model, limit }; }
        ~Scope() { active = previous_; }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
    };

    inline uint32_t Capacity(const void* model, uint32_t available)
    {
        return model && active.model == model && active.limit
            ? std::min(available, active.limit) : available;
    }
}
