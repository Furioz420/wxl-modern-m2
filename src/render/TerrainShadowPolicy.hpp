// Copyright (C) 2026 WarcraftXL. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "../compat/CharacterCopyCache.hpp"
#include <d3d9.h>
#include <bit>
#include <cmath>

namespace wxl::modern::terrainshadow
{
    // Exact locally captured receiver identities, not embedded client/upstream shader code.
    // Contracts are independently checked against the local disassembly. Sparse taps avoid
    // overwriting constants used by lighting/material logic in other receiver families.
    struct Contract { unsigned strengthRegister=12, component=0, firstTap=3, tapMask=255, firstSampler=5, samplerCount=4; };
    inline bool Patch(std::vector<DWORD>& words,Contract* contract=nullptr,float strength=1.35f)
    {
        if(!std::isfinite(strength) || strength<.5f || strength>2) return false;
        struct Identity { size_t bytes; const char* hash; Contract contract{}; };
        constexpr Identity known[]{
            {2324,"ad1576859df22534e5d5b2457add725cad784c7752ec9d082599175e82526a72"},
            {3564,"6655b6cc3504ddeb5f4a5ed5f94c092acabf6f7083ce8ac377196cee38f13d39"},
            {2264,"934a1948e46395f3d0e97f8fb123f224d3ff92ab84080ebdb901f71a7f22a581"},
            {2176,"bc5fab44f937a13f3e6d67a0261f3757457ebe452175b7849a8b9f5afe396552"},
            {1588,"24c2c665054205b408bba34848906b73fadb33aba72abff3a1545d8804ff3a9b",{13,0,5,255,4,1}},
            {2412,"259824c48d1717aef69a51a09b04bd519a01e75afe4c3eec34b719d1b01f5bb1",{8,1,5,85,4,4}},
            {2424,"a48b4b9b7d93ca48a144118d5dbe280c0507e2f57aa614dbc836fc28c08f0c13",{8,1,5,85,4,4}},
            {2380,"96596bba5e6c45d631a7d304efa75351274463b4a538eab29dbe6bf2197cfc39",{8,1,5,85,4,4}},
            {2396,"35cf6ac83a78a46455f3d7a6a5ca5727bd5d73c13a65d569df468614a0576b1c",{8,1,5,85,4,4}},
            {2452,"e836057fdcef86cada3f3405158ac95be4dc8dea387036ff1f276067cae9443f",{8,1,5,85,4,4}},
            {1656,"83f1ea0fa3aab363b1889bb7fc9111e61746aad6a35b8a18f8042367ad8ec855",{13,0,5,255,4,1}},
            {2480,"a2f2f2cef47ae1c238089a672cacf169ae28404948a455e4663f9a4a1e5a601e",{8,1,5,85,4,4}},
            {1556,"d54bbc199e7672639b8017db5c05b8b874079c1120702fe4605ab4a32880f26b",{13,0,5,255,4,1}},
            {2452,"71a3620fc1c944166fac750bd447ae24b674f87c30968da1aa9be5cf9251e447",{8,1,5,85,4,4}},
            {1600,"6c2dd97dd79bddbb6f675b5e2e29b529f846cf0b89b7ae63be4cbdb9a68e6ee0",{13,0,5,255,4,1}},
            {2492,"bdb7dbf44c4376ceb0d7fff1ff886ff9c70c7b3808f3ebece3aac44f2faebfc5",{8,1,5,85,4,4}},
            {1616,"dd49075f4a67a5b4763c264b273a9c204d322b901eb809909526419465ff7e31",{13,0,5,255,4,1}},
            {1628,"b68d03b3bd7e72207b15c477524b9c5a39a4e159b41b8a78ec3f7391b64f60c5",{13,0,5,255,4,1}},
            {2408,"e761d8f215688ac549a49ca4faf20ec15d0ea3e0c8084985f0a8732bcdfa7937",{8,1,5,85,4,4}},
            {2516,"dcacf23519ce8321756c9e8d365760ccade93213cd1683b444551c3b133d1eec",{8,1,5,85,4,4}}};
        const size_t bytes=words.size()*sizeof(DWORD);
        if(words.empty() || words.front()!=D3DPS_VERSION(3,0)) return false;
        bool eligible=false; for(const auto& candidate:known) eligible|=bytes==candidate.bytes;
        if(!eligible) return false;
        wxl_copy_cache::Digest digest; wxl_copy_cache::Key key{};
        if(!digest.Add(words.data(),bytes) || !digest.Finish(key)) return false;
        constexpr char hex[]="0123456789abcdef";
        const Identity* identity=nullptr;
        for(const auto& candidate:known)
        {
            if(bytes!=candidate.bytes) continue;
            bool equal=true;
            for(size_t i=0;i<key.size();++i)
                equal &= candidate.hash[i*2]==hex[key[i]>>4] && candidate.hash[i*2+1]==hex[key[i]&15];
            if(equal) { identity=&candidate; break; }
        }
        if(!identity) return false;
        const auto& c=identity->contract;
        size_t found=0;
        for(size_t i=1;i<words.size();)
        {
            DWORD token=words[i],op=token&D3DSI_OPCODE_MASK;
            if(op==D3DSIO_END) break;
            size_t length=op==D3DSIO_COMMENT ? ((token>>16)&0x7fff) : ((token>>24)&15);
            // ELSE/ENDIF have no operands. Advancing over the opcode still makes progress.
            if(i+length>=words.size()) return false;
            if(op==D3DSIO_DEF && length==5 && words[i+1]==(0xa00f0000u|c.strengthRegister))
            {
                if(found || words[i+2+c.component]!=std::bit_cast<DWORD>(.3f) || words[i+3+c.component]!=std::bit_cast<DWORD>(.7f)) return false;
                found=i+2+c.component;
            }
            i+=length+1;
        }
        if(!found) return false;
        words[found]=std::bit_cast<DWORD>(strength==1.35f ? .405f : .3f*strength);
        words[found+1]=std::bit_cast<DWORD>(strength==1.35f ? .595f : 1.f-.3f*strength);
        if(contract) *contract=c;
        return true;
    }
    inline bool Soften(const float* original,float* output,unsigned mask=255,float softness=1.35f)
    {
        if(!std::isfinite(softness) || softness<1 || softness>2) return false;
        for(unsigned i=0;i<32;++i)
            if((mask&(1u<<(i/4))) && (!std::isfinite(original[i]) || std::abs(original[i])>.01f || (i%4>=2 && original[i]!=0))) return false;
        std::memcpy(output,original,32*sizeof(float));
        for(unsigned i=0;i<32;++i) if(i%4<2 && (mask&(1u<<(i/4)))) output[i]*=softness;
        return true;
    }
}
