// Versioned, opt-in projection of verified retail presentation into native Wrath kits.
// No gameplay fields, global animation substitutions, or disk writes. GPLv3.
#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

namespace wxl::spell::presentation
{
    using Bytes = std::span<const uint8_t>;
    inline uint32_t Word(Bytes b, size_t off) noexcept
    { uint32_t v = 0; if (off <= b.size() && b.size() - off >= 4) std::memcpy(&v, b.data()+off, 4); return v; }
    struct Table
    {
        Bytes bytes{};
        uint32_t count = 0, fields = 0;
        size_t strings = 0;
        bool Open(Bytes b, uint32_t f) noexcept
        {
            *this = {};
            if (b.size() < 20 || Word(b,0) != 0x43424457 || !Word(b,4) ||
                Word(b,4) > 1000000 || Word(b,8) != f || Word(b,12) != f*4 || !Word(b,16)) return false;
            const uint64_t end = 20ull + uint64_t(Word(b,4))*f*4;
            if (end + Word(b,16) != b.size()) return false;
            bytes=b; count=Word(b,4); fields=f; strings=static_cast<size_t>(end); return true;
        }
        uint32_t At(uint32_t row, uint32_t col) const noexcept
        { return Word(bytes,20 + (size_t(row)*fields + col)*4); }
        bool TextEquals(uint32_t offset, std::string_view expected) const noexcept
        {
            if (offset >= bytes.size()-strings) return false;
            const auto tail=bytes.subspan(strings+offset);
            if (tail.size() <= expected.size() || tail[expected.size()] != 0) return false;
            for (size_t i=0;i<expected.size();++i)
            {
                auto c=tail[i]; if (c=='/') c='\\'; if (c>='A' && c<='Z') c+=32;
                if (c!=static_cast<uint8_t>(expected[i])) return false;
            }
            return true;
        }
    };
    inline constexpr std::array<uint32_t,4> kRanks{50796,59170,59171,59172};
    inline constexpr uint32_t kVisual=23335, kPrecast=23461, kRelease=23462, kImpact=23460;

    inline bool VerifySpells(Bytes bytes) noexcept
    {
        Table t; if (!t.Open(bytes,234)) return false;
        std::array<unsigned,4> seen{};
        for (uint32_t r=0;r<t.count;++r)
        {
            size_t rank=0; while (rank<kRanks.size() && t.At(r,0)!=kRanks[rank]) ++rank;
            if (rank<kRanks.size())
            { if (++seen[rank]!=1 || t.At(r,131)!=kVisual || t.At(r,132)!=0) return false; }
            else if (t.At(r,131)==kVisual || t.At(r,132)==kVisual) return false;
        }
        for (auto n:seen) if (n!=1) return false;
        return true;
    }
    inline bool VerifyVisuals(Bytes bytes) noexcept
    {
        Table t; if (!t.Open(bytes,32)) return false;
        unsigned seen=0;
        for (uint32_t r=0;r<t.count;++r)
        {
            const bool target=t.At(r,0)==kVisual;
            if (target && (++seen!=1 || t.At(r,1)!=kPrecast || t.At(r,2)!=kRelease || t.At(r,3)!=kImpact)) return false;
            // Conservative: reject any other occurrence, even in an unrelated numeric field.
            for (uint32_t c=1;c<32;++c)
                if (t.At(r,c)==kRelease && (!target || c!=2)) return false;
        }
        return seen==1;
    }
    inline bool VerifyAttachments(Bytes bytes) noexcept
    {
        Table t; if (!t.Open(bytes,10)) return false;
        for (uint32_t r=0;r<t.count;++r) if (t.At(r,1)==kRelease) return false;
        return true;
    }
    inline bool VerifyEffects(Bytes bytes) noexcept
    {
        Table t; if (!t.Open(bytes,7)) return false;
        unsigned seen=0;
        for (uint32_t r=0;r<t.count;++r) if (t.At(r,0)==12630)
            if (++seen!=1 || t.At(r,4)!=0x3f800000 ||
                !t.TextEquals(t.At(r,2),"spells\\cfx_warlock_chaosbolt_precasthand.m2")) return false;
        return seen==1;
    }
    // An exact expected row includes absent char procedures, animation 53 and sound 18045.
    // Only left/right hand fields 6/7 change; failure leaves the caller's output untouched.
    inline bool ProjectRelease(Bytes bytes, std::vector<uint8_t>& output)
    {
        Table t; if (!t.Open(bytes,38)) return false;
        std::array<uint32_t,38> expected{};
        expected[0]=kRelease; expected[1]=0xffffffff; expected[2]=53; expected[15]=18045;
        for (size_t c=17;c<=20;++c) expected[c]=0xffffffff;
        unsigned seen=0; uint32_t selected=0;
        for (uint32_t r=0;r<t.count;++r) if (t.At(r,0)==kRelease)
        {
            if (++seen!=1) return false;
            selected=r;
            for (uint32_t c=0;c<38;++c) if (t.At(r,c)!=expected[c]) return false;
        }
        if (seen!=1) return false;
        auto result=std::vector<uint8_t>(bytes.begin(),bytes.end());
        const uint32_t effect=12630;
        for (size_t c: {6u,7u}) std::memcpy(result.data()+20+(size_t(selected)*38+c)*4,&effect,4);
        output.swap(result); return true;
    }
}
