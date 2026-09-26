// The submesh group vocabulary: what a geoset id's group actually means, so the geoset decision and
// its panel can print something a reader can act on instead of a bare number.
// Copyright (C) 2026 WarcraftXL
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

#include "../ExtensionApi.hpp"

#include <cstdint>

namespace
{
    /// One named group, keyed by `id / 100`. Ranges not listed here have no known name and are left
    /// that way rather than guessed.
    struct GroupName { uint32_t group; const char* name; };
    constexpr GroupName kGroupNames[] = {
        { 0,  "Hair" },            // group 0 is also id 0 alone, handled separately below
        { 1,  "Facial1" },
        { 2,  "Facial2" },
        { 3,  "Facial3" },
        { 4,  "Gloves" },
        { 5,  "Boots" },
        { 6,  "Shirt" },
        { 7,  "Ears" },
        { 8,  "Sleeves" },
        { 9,  "Kneepads" },
        { 10, "ShirtDoublet" },
        { 11, "Pants" },
        { 12, "Tabard" },
        { 13, "Robe" },
        { 14, "Loincloth" },
        { 15, "Cape" },
        { 16, "FacialJewelry" },
        { 17, "EyeEffects" },
        { 18, "Belt" },
        { 19, "BoneTail" },
        { 20, "Toes" },
        { 21, "Skull" },
        { 22, "Torso" },
        { 23, "Hands" },
        { 24, "Horns" },
        { 25, "Facewear" },
        { 26, "Shoulders" },
        { 27, "Helm" },
        { 28, "ArmUpper" },
        { 29, "ArmsReplace" },
        { 30, "LegsReplace" },
        { 31, "FeetReplace" },
        { 32, "HeadSwap" },
        { 33, "Eyes" },
        { 34, "Eyebrows" },
        { 35, "Piercings" },
        { 36, "Necklaces" },
        { 37, "Headdress" },
        { 38, "DrTail" },
        { 39, "MiscAccessory" },
        { 40, "MiscFeature" },
        { 41, "Noses" },
        { 42, "HairDecoration" },
        { 43, "HornDecoration" },
        { 44, "BodySize" },
        { 51, "EyeGlow2" },
    };
}
namespace wxl_modern_m2
{
    const char* GeosetGroupName(uint32_t geosetId)
    {
        // Id 0 and ids 1..99 both divide to group 0, but the format does not mean the same thing by
        // them: 0 alone is the base body, while 1..99 is the Hair group. Naive `id / 100` indexing
        // would call the base body "Hair", which is why 0 is answered here before the table below is
        // ever consulted.
        if (geosetId == 0) return "Skin";

        const uint32_t group = geosetId / 100;
        for (const GroupName& entry : kGroupNames)
            if (entry.group == group) return entry.name;
        return nullptr;
    }
}
