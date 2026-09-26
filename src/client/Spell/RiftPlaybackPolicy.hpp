// Exact-source preview playback policy. No native calls or global sequence rewrites. GPLv3.
#pragma once
#include "engine/assets/shared/models/m2/M2Format.hpp"
#include <cstdint>
#include <span>

namespace wxl::modern::riftplayback {
enum class Phase : uint8_t { Waiting, Startup, Hold, Disabled };
struct State { Phase phase=Phase::Waiting; uint32_t startedAt=0; };
inline constexpr uint32_t kNone=UINT32_MAX, kStartup=213, kHold=158, kStartupMs=1000;
inline bool SequenceContract(std::span<const wxl::structure::m2::M2Sequence> seq) noexcept {
    if(seq.size()!=4)return false;
    constexpr uint16_t ids[]{213,0,158,159};
    constexpr uint32_t durations[]{1000,1000,333,700},flags[]{0x8a1,0x8e0,0x8a1,0xaa1};
    constexpr uint16_t aliases[]{1,0,2,3};
    for(unsigned i=0;i<4;++i)
        if(seq[i].id!=ids[i] || seq[i].variationIndex!=0 || seq[i].duration!=durations[i] ||
           seq[i].flags!=flags[i] || seq[i].aliasNext!=aliases[i])return false;
    return true;
}
inline uint32_t Request(const State& state,uint32_t now) noexcept {
    if(state.phase==Phase::Waiting)return kStartup;
    if(state.phase==Phase::Startup && uint32_t(now-state.startedAt)>=kStartupMs)return kHold;
    return kNone;
}
inline bool Commit(State& state,uint32_t id,uint32_t now) noexcept {
    if(id==kStartup && state.phase==Phase::Waiting){state={Phase::Startup,now};return true;}
    if(id==kHold && state.phase==Phase::Startup && Request(state,now)==kHold){state.phase=Phase::Hold;return true;}
    return false;
}
}
