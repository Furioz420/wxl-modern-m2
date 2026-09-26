#pragma once
#include "DragonHornPolicy.hpp"
#include "wxl/AppearanceApi.h"
#include <cstring>
#include <limits>
namespace wxl_modern_m2::gilnean {
inline bool Selectable(const WXL_AppearanceApi* t, const WXL_ChrChoice& c)
{
    if (dragon::RequiredHorns(c.id) != c.id) return false;
    const char* name = t->ChoiceName(c.id);
    return !name || std::strcmp(name, "Transmog");
}
inline uint32_t RequiredHornIndex(const WXL_AppearanceApi* t, uint32_t option, uint32_t index)
{
    WXL_ChrChoice choice{};
    if (!t->ChoiceAt(option, index, &choice)) return index;
    const uint32_t required = dragon::RequiredHorns(choice.id);
    if (required == choice.id) return index;
    for (uint32_t i = 0; i < t->ChoiceCount(option); ++i)
        if (t->ChoiceAt(option, i, &choice) && choice.id == required) return i;
    return index;
}
inline int NextSelectable(const WXL_AppearanceApi* t, uint32_t option, int current, int delta)
{
    const int count = int(t->ChoiceCount(option));
    if (!count) return -1;
    current = current >= 0 && current < count ? current : 0;
    for (int n = 0; n < count; ++n)
    {
        current = (current + (delta > 0 ? 1 : count - 1)) % count;
        WXL_ChrChoice c{};
        if (t->ChoiceAt(option, current, &c) && Selectable(t, c)) return current;
    }
    return -1;
}
// These legacy Gilnean choices have no DB2 swatch. Median opaque RGB measured from
// their authored diffuse hair textures (male FDID1042855..9, female1002498..502).
// Key by choice ID, never by ordinal; the Transmog placeholder is intentionally excluded.
inline uint32_t EffectiveSwatch(const WXL_ChrChoice& choice)
{
    if (choice.swatchColor) return choice.swatchColor;
    switch (choice.id)
    {
        case 2427: return 0xff181410;
        case 2428: return 0xff2d2208;
        case 2429: return 0xff482422;
        case 2430: return 0xff421615;
        case 2431: return 0xff5d2408;
        case 2515: return 0xff1d1915;
        case 2516: return 0xff2d0d12;
        case 2517: return 0xff3d2020;
        case 2518: return 0xff5a1200;
        case 2519: return 0xff4d2400;
        default: return 0;
    }
}
// Compare authored swatches, never assume race-specific choice ordinals mean the same thing.
inline uint32_t ColorDistance(uint32_t a, uint32_t b)
{
    uint32_t result = 0;
    for (unsigned shift = 0; shift < 24; shift += 8)
    { int d = int((a >> shift) & 255) - int((b >> shift) & 255); result += d * d; }
    return result;
}
inline int MatchChoice(const WXL_AppearanceApi* t, const WXL_ChrChoice& from, uint32_t option)
{
    if (!Selectable(t, from)) return -1;
    int best = -1; uint32_t distance = std::numeric_limits<uint32_t>::max();
    const char* name = t->ChoiceName(from.id);
    for (uint32_t i = 0; i < t->ChoiceCount(option); ++i)
    {
        WXL_ChrChoice candidate{}; if (!t->ChoiceAt(option, i, &candidate)) continue;
        if (!Selectable(t, candidate)) continue;
        const char* other = t->ChoiceName(candidate.id);
        // Numbered/empty placeholders are not cross-form semantic identities.
        if (name && *name && (*name < '0' || *name > '9') &&
            std::strncmp(name, "New ", 4) && other && !std::strcmp(name, other)) return int(i);
        if (!EffectiveSwatch(from) || !EffectiveSwatch(candidate)) continue;
        const uint32_t d = ColorDistance(EffectiveSwatch(from), EffectiveSwatch(candidate));
        if (d < distance) { distance = d; best = int(i); }
    }
    return best;
}
inline bool SameAxis(const char* a, const char* b)
{
    if (!a || !b) return false;
    return !std::strcmp(a,b) || (!std::strcmp(a,"Fur Color") && !std::strcmp(b,"Hair Color")) ||
        (!std::strcmp(b,"Fur Color") && !std::strcmp(a,"Hair Color")) ||
        (!std::strcmp(a,"Fur Color") && !std::strcmp(b,"Skin Color")) ||
        (!std::strcmp(b,"Fur Color") && !std::strcmp(a,"Skin Color"));
}
inline uint32_t Luminance(uint32_t rgb)
{ return 54 * ((rgb >> 16) & 255) + 183 * ((rgb >> 8) & 255) + 19 * (rgb & 255); }
// Fur and human skin are different palettes. Match relative lightness rather than
// literal RGB (grey fur has no literal skin equivalent). Gilnean skin is a legacy
// light-to-dark ramp. This policy is approximate and scoped to these two pairs.
inline int MatchSkinTone(const WXL_AppearanceApi* t, const WXL_ChrChoice& from,
                         uint32_t source, uint32_t target)
{
    const bool toSkin = target == 213 || target == 218;
    const uint32_t fur = toSkin ? source : target, skin = toSkin ? target : source;
    uint32_t low = 65535, high = 0, skinCount = 0, sourceRank = 0;
    for (uint32_t i = 0; i < t->ChoiceCount(fur); ++i)
    {
        WXL_ChrChoice c{}; if (!t->ChoiceAt(fur,i,&c) || !Selectable(t,c) || !EffectiveSwatch(c)) continue;
        const auto value = Luminance(EffectiveSwatch(c));
        if (value < low) low = value; if (value > high) high = value;
    }
    for (uint32_t i = 0; i < t->ChoiceCount(skin); ++i)
    {
        WXL_ChrChoice c{}; if (!t->ChoiceAt(skin,i,&c) || !Selectable(t,c)) continue;
        if (c.id == from.id) sourceRank = skinCount;
        ++skinCount;
    }
    if (high <= low || !skinCount || !Selectable(t,from)) return -1;
    if (toSkin)
    {
        const uint32_t value = Luminance(EffectiveSwatch(from));
        const uint32_t rank = uint32_t((uint64_t(high - value) * (skinCount - 1) + (high-low)/2) / (high-low));
        uint32_t valid = 0;
        for (uint32_t i = 0; i < t->ChoiceCount(skin); ++i)
        { WXL_ChrChoice c{}; if (t->ChoiceAt(skin,i,&c) && Selectable(t,c) && valid++ == rank) return int(i); }
    }
    else
    {
        const int wanted = int(high - (skinCount > 1 ? uint64_t(sourceRank) * (high-low)/(skinCount-1) : 0));
        int best = -1, distance = 65536;
        for (uint32_t i = 0; i < t->ChoiceCount(fur); ++i)
        {
            WXL_ChrChoice c{}; if (!t->ChoiceAt(fur,i,&c) || !Selectable(t,c) || !EffectiveSwatch(c)) continue;
            int d = int(Luminance(EffectiveSwatch(c))) - wanted; if (d < 0) d = -d;
            if (d < distance) { distance = d; best = int(i); }
        }
        return best;
    }
    return -1;
}
// The installed legacy Gilnean catalogs have unnamed face/hair choices and no
// authored cross-form relations. For these specific pairs only, spread the source
// selection over the destination catalog. This is a deterministic preview fallback,
// not a claim that equal ordinals identify equivalent retail hairstyles.
inline int MatchLinkedChoice(const WXL_AppearanceApi* t, const WXL_ChrChoice& from,
                             uint32_t sourceOption, uint32_t targetOption)
{
    if ((sourceOption == 203 && targetOption == 213) || (sourceOption == 208 && targetOption == 218) ||
        (sourceOption == 213 && targetOption == 203) || (sourceOption == 218 && targetOption == 208))
        return MatchSkinTone(t,from,sourceOption,targetOption);
    if (int exact = MatchChoice(t, from, targetOption); exact >= 0) return exact;
    uint32_t a = sourceOption, b = targetOption;
    if (a > b) { const uint32_t swap = a; a = b; b = swap; }
    if (!((a == 204 && b == 214) || (a == 205 && b == 215) ||
          (a == 207 && b == 217) || (a == 209 && b == 219) ||
          (a == 210 && b == 220))) return -1;
    uint32_t sourceRank = 0, sourceCount = 0, targetCount = 0;
    bool found = false;
    for (uint32_t i = 0; i < t->ChoiceCount(sourceOption); ++i)
    {
        WXL_ChrChoice c{}; if (!t->ChoiceAt(sourceOption, i, &c)) continue;
        const char* name = t->ChoiceName(c.id);
        if (name && !std::strcmp(name, "Transmog")) continue;
        if (c.id == from.id) { sourceRank = sourceCount; found = true; }
        ++sourceCount;
    }
    for (uint32_t i = 0; i < t->ChoiceCount(targetOption); ++i)
    {
        WXL_ChrChoice c{}; if (!t->ChoiceAt(targetOption, i, &c)) continue;
        const char* name = t->ChoiceName(c.id);
        if (!name || std::strcmp(name, "Transmog")) ++targetCount;
    }
    if (!found || !targetCount) return -1;
    const uint32_t rank = sourceCount > 1
        ? uint32_t((uint64_t(sourceRank) * (targetCount - 1) + (sourceCount - 1) / 2) / (sourceCount - 1)) : 0;
    uint32_t valid = 0;
    for (uint32_t i = 0; i < t->ChoiceCount(targetOption); ++i)
    {
        WXL_ChrChoice c{}; if (!t->ChoiceAt(targetOption, i, &c)) continue;
        const char* name = t->ChoiceName(c.id);
        if (name && !std::strcmp(name, "Transmog")) continue;
        if (valid++ == rank) return int(i);
    }
    return -1;
}
}
