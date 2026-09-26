#pragma once
#include <array>
#include <cstdint>
#include <cstddef>

namespace wxl_memory {
// Allocation-free telemetry. Caller serializes access; never owns/frees pointers.
template<size_t Capacity> class NativeAllocationStats {
    static_assert(Capacity && !(Capacity & (Capacity - 1)));
    struct Slot { uintptr_t pointer = 0; uint32_t bytes = 0; };
    std::array<Slot, Capacity> slots_{};
    static size_t Hash(uintptr_t p) { return ((p >> 4) * uintptr_t(2654435761u)) & (Capacity-1); }
public:
    uint64_t bytes = 0, missed = 0;
    size_t count = 0;
    bool Add(uintptr_t p, uint32_t n) noexcept {
        if (p <= 1) return false;
        size_t reuse = Capacity;
        for (size_t i=0;i<Capacity;++i) {
            const size_t index=(Hash(p)+i)&(Capacity-1);
            auto& s=slots_[index];
            if (s.pointer==p) { bytes-=s.bytes; s.bytes=n; bytes+=n; return true; }
            if (s.pointer==1 && reuse==Capacity) reuse=index;
            if (s.pointer==0) { if(reuse==Capacity)reuse=index; break; }
        }
        if(reuse==Capacity) { ++missed; return false; }
        slots_[reuse]={p,n}; bytes+=n; ++count; return true;
    }
    void Remove(uintptr_t p) noexcept {
        if(p<=1)return;
        for(size_t i=0;i<Capacity;++i) {
            auto& s=slots_[(Hash(p)+i)&(Capacity-1)];
            if(!s.pointer)return;
            if(s.pointer==p) { bytes-=s.bytes; --count; s={1,0}; return; }
        }
    }
};
}
