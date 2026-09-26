// Narrow native animation bridge for the verified Chaos Bolt loop/release. GPLv3.
#pragma once
#include "client/Spell/LegacyPresentation.hpp"
#include "engine/assets/shared/models/m2/M2Format.hpp"
#include <atomic>

namespace wxl::spell::presentation
{
    inline std::atomic<bool> animationHooksReady{false};
    inline std::atomic<bool> animationProjectionActive{false};
    inline constexpr std::array<uint32_t,2> kPoseIds{922,926};
    inline constexpr std::array<uint32_t,2> kPoseFallbacks{51,53};
    inline constexpr const char* kPoseNames[]{"wxl_chaos_loop_v1","wxl_chaos_release_v1"};
    inline int PoseIndex(uint32_t id) noexcept {return id==922?0:id==926?1:-1;}
    inline void PutWord(std::vector<uint8_t>& b,size_t o,uint32_t v) {std::memcpy(b.data()+o,&v,4);}

    // Conservative first backend: every variant must have embedded tracks. No streaming,
    // alias chains or blend-time guessing here; unsupported models resolve to native 51/53.
    inline bool EmbeddedPose(std::span<const wxl::structure::m2::M2Sequence> seqs,uint32_t id) noexcept
    {
        bool found=false;
        for(const auto& s:seqs) if(s.id==id)
        {if(!(s.flags&0x20) || (s.flags&0x40) || !s.duration) return false; found=true;}
        return found;
    }
    inline bool EmbeddedPoseFamily(std::span<const wxl::structure::m2::M2Sequence> seqs) noexcept
    {return EmbeddedPose(seqs,922) && EmbeddedPose(seqs,926);}
    inline uint32_t SelectPose(uint32_t requested,bool embeddedFamily) noexcept
    {const int i=PoseIndex(requested);return i>=0 && !embeddedFamily ? kPoseFallbacks[i] : requested;}

    inline bool ProjectAnimationData(Bytes input,std::vector<uint8_t>& output)
    {
        Table t;if(!t.Open(input,8))return false;
        std::array<std::array<uint32_t,8>,2> rows{};
        std::array<unsigned,2> seen{};
        for(uint32_t r=0;r<t.count;++r)
        {
            if(PoseIndex(t.At(r,0))>=0)return false; // Never overwrite someone else's rows.
            for(size_t i=0;i<2;++i)if(t.At(r,0)==kPoseFallbacks[i])
            {
                if(++seen[i]!=1)return false;
                for(uint32_t c=0;c<8;++c)rows[i][c]=t.At(r,c);
                if(rows[i][2]!=4 || rows[i][3]!=(i?264u:256u) || rows[i][4]!=2 ||
                   rows[i][5]!=(i?54u:52u) || rows[i][6]!=kPoseFallbacks[i] || rows[i][7]!=0)return false;
            }
        }
        if(seen[0]!=1 || seen[1]!=1)return false;
        auto result=std::vector<uint8_t>(input.begin(),input.begin()+t.strings);
        std::vector<uint8_t> strings(input.begin()+t.strings,input.end());
        for(size_t i=0;i<2;++i)
        {
            auto& row=rows[i];row[0]=kPoseIds[i];row[1]=static_cast<uint32_t>(strings.size());
            row[5]=kPoseFallbacks[i]; // Wrath presentation fallback, not retail's entire graph.
            const char* name=kPoseNames[i];strings.insert(strings.end(),name,name+std::strlen(name)+1);
            const auto* data=reinterpret_cast<const uint8_t*>(row.data());result.insert(result.end(),data,data+32);
        }
        result.insert(result.end(),strings.begin(),strings.end());
        PutWord(result,4,t.count+2);PutWord(result,16,static_cast<uint32_t>(strings.size()));
        output.swap(result);return true;
    }
    // Input is the original native kit table; release hand projection remains required.
    inline bool ProjectPoseKits(Bytes input,std::vector<uint8_t>& output)
    {
        std::vector<uint8_t> result;if(!ProjectRelease(input,result))return false;
        Table t;if(!t.Open(input,38))return false;
        unsigned seen=0;
        for(uint32_t r=0;r<t.count;++r)
        {
            if(t.At(r,0)==kPrecast)
            {
                if(++seen!=1)return false;
                std::array<uint32_t,38> expected{};expected[0]=kPrecast;expected[1]=~0u;expected[2]=51;
                expected[6]=expected[7]=12630;expected[15]=18044;
                for(size_t c=17;c<=20;++c)expected[c]=~0u;
                for(uint32_t c=0;c<38;++c)if(t.At(r,c)!=expected[c])return false;
                PutWord(result,20+(size_t(r)*38+2)*4,922);
            }
            if(t.At(r,0)==kRelease)PutWord(result,20+(size_t(r)*38+2)*4,926);
        }
        if(seen!=1)return false;
        output.swap(result);return true;
    }
    inline bool VerifyPoseVisuals(Bytes bytes) noexcept
    {
        if(!VerifyVisuals(bytes))return false;
        Table t;if(!t.Open(bytes,32))return false;
        for(uint32_t r=0;r<t.count;++r)for(uint32_t c=1;c<32;++c)
            if(t.At(r,c)==kPrecast && (t.At(r,0)!=kVisual || c!=1))return false;
        return true;
    }
    inline bool VerifyPoseAttachments(Bytes bytes) noexcept
    {
        if(!VerifyAttachments(bytes))return false;
        Table t;if(!t.Open(bytes,10))return false;
        for(uint32_t r=0;r<t.count;++r)if(t.At(r,1)==kPrecast)return false;
        return true;
    }
}
