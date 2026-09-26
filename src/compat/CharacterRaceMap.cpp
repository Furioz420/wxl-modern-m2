#include "../ExtensionApi.hpp"

namespace wxl_modern_m2
{
    bool RetailCharacterCanaryAllows(uint32_t raceId, uint32_t sex)
    {
        // Eredar and Broken deliberately use their private legacy models and CharSections rows.
        // Retail has no separate Eredar actor (Man'ari is a Draenei choice family), while Retail
        // race 14 describes the modern Broken body. Feeding either old mesh a Retail sheet gives it
        // another actor's UV/material contract.
        if (raceId == 16 || raceId == 23) return false;
        static const uint32_t raceFilter = ConfigU32("WXL_M2_RETAIL_CHARACTER_RACE", 0, 0, 255);
        static const uint32_t sexFilter = ConfigU32("WXL_M2_RETAIL_CHARACTER_SEX", 2, 0, 2);
        return (!raceFilter || raceId == raceFilter) && (sexFilter > 1 || sex == sexFilter);
    }

    uint32_t RetailCharacterRace(uint32_t raceId)
    {
        // These are CLIENT ChrRaces ids, not Retail ids. Several private rows deliberately occupy
        // numbers that mean a different race in Retail (22 Pandaren-Horde vs Retail Worgen, 28
        // Murloc vs Retail Highmountain, 30 Dracthyr-Horde vs Retail Lightforged). Falling through
        // numerically therefore composes an unrelated body and enumerates somebody else's options.
        switch (raceId)
        {
            case 1:  return 1;  // Human
            case 2:  return 2;  // Orc
            case 3:  return 3;  // Dwarf
            case 4:  return 4;  // Night Elf
            case 5:  return 5;  // Undead
            case 6:  return 6;  // Tauren
            case 7:  return 7;  // Gnome
            case 8:  return 8;  // Troll
            case 9:  return 35; // Vulpera
            case 10: return 10; // Blood Elf
            case 11: return 11; // Draenei
            case 12: return 22; // Worgen; Gilnean preview deferred until it has a distinct CMO owner
            case 13: return 27; // Nightborne
            case 14: return 17; // Tuskarr
            case 15: return 29; // Void Elf
            case 16: return 0;  // Eredar keeps its private actor and legacy customization contract
            case 17: return 76; // Dracthyr Horde defaults to visage
            case 18: return 31; // Zandalari Troll
            case 19: return 0;  // Ogre: creature/custom family, no verified playable recipe
            case 20: return 30; // Lightforged Draenei
            case 21: return 9;  // Goblin
            case 22: return 26; // Pandaren (Horde)
            case 23: return 14; // Broken
            case 24: return 85; // Custom Alliance Illidari slot now serves Earthen
            case 25: return 13; // Naga
            case 26: return 25; // Pandaren (Alliance)
            case 27: return 86; // Custom Horde Illidari slot now serves Harronir
            case 28: return 0;  // Murloc: creature family, not Retail Highmountain Tauren
            case 29: return 34; // Dark Iron Dwarf
            case 30: return 75; // Dracthyr Alliance defaults to visage
            case 31: return 32; // Kul Tiran
            default: return 0;  // Not a configured playable client row; never pass through a collision.
        }
    }
}
