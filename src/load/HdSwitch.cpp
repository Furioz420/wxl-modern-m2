// Serves the reworked character models in place of the ones the stock client asks for.
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

// MECHANISM:
//
//   The stock client has no name for the reworked models. It asks for the race model it has always
//   asked for, and it derives every companion from that same name: the skin profile, each streamed
//   animation, and the skeleton this reader looks for. So one rewrite at the point of asking moves
//   the whole family together, and nothing downstream has to be told.
//
//   WHICH model to serve is not decided here and is not spelled: the tables name it. A stock path
//   says which race and sex is being asked for, and ChrRaceXChrModel leads from that pair to a
//   ChrModel, its display entry and the model's own FileDataID. Whatever file that resolves to is the
//   answer, whether or not its name happens to carry a suffix.
//
//   Only the STEM of that answer is taken, because the companions are derived names the client builds
//   itself and there is no table row for any of them:
//
//       trollfemale.m2            -> <served stem>.m2
//       trollfemale00.skin        -> <served stem>00.skin
//       trollfemale0060-00.anim   -> <served stem>0060-00.anim
//       trollfemale.skel          -> <served stem>.skel
//
//   The stem is the filename up to its first digit or its dot, which is exactly the boundary all four
//   forms share.
//
//   Two things hold it to the models. A path the race table does not recognise is declined outright,
//   which keeps the companions, the drakes and the rostrum props out of it. And of what remains, only
//   a name the served one CONTINUES is answered: a race's own textures live in the same directory and
//   their names begin with the model's, so a rule that fired on the directory alone would answer a
//   request for a skin file with a name assembled out of the model's, and nobody ships that file.

#include "../ExtensionApi.hpp"
#include "HdAliasPolicy.hpp"

#include "wxl/AppearanceApi.h"
#include "wxl/StorageApi.h"

#include <cctype>
#include <cstdint>
#include <cstring>

namespace
{
    const WXL_AppearanceApi* g_appearance = nullptr;
    uint32_t g_raceFilter = 0; // reported at install; the shared guard owns the decision
    uint32_t g_sexFilter = 2;

    const WXL_AppearanceApi* Appearance()
    {
        if (!g_appearance)
            g_appearance = static_cast<const WXL_AppearanceApi*>(
                wxl_modern_m2::g_api->GetInterface("wxl.appearance", WXL_APPEARANCE_API_VERSION));
        return g_appearance;
    }

    /// Case-insensitive compare of the n bytes at a against b. Shipped paths differ in case between
    /// what the client asks for and what the tables name, and the two are the same file.
    bool MatchesFold(const char* a, const char* b, size_t n)
    {
        for (size_t i = 0; i < n; ++i)
            if (std::tolower(static_cast<unsigned char>(a[i]))
                != std::tolower(static_cast<unsigned char>(b[i]))) return false;
        return true;
    }

    bool MatchesPathFold(const char* a, const char* b, size_t n)
    {
        for (size_t i = 0; i < n; ++i)
        {
            const unsigned char left = static_cast<unsigned char>(a[i]);
            const unsigned char right = static_cast<unsigned char>(b[i]);
            if ((left == '\\' || left == '/') && (right == '\\' || right == '/')) continue;
            if (std::tolower(left) != std::tolower(right)) return false;
        }
        return true;
    }

    /// The last separator in a path, or the start when there is none.
    const char* FileNameOf(const char* path)
    {
        const char* name = path;
        for (const char* p = path; *p; ++p)
            if (*p == '\\' || *p == '/') name = p + 1;
        return name;
    }

    /// Where the stem ends: the first digit or the first dot. Both endings appear across the four
    /// companion forms, and neither ever occurs inside a stem itself.
    size_t StemLength(const char* name)
    {
        size_t n = 0;
        while (name[n] && name[n] != '.' && !std::isdigit(static_cast<unsigned char>(name[n]))) ++n;
        return n;
    }

    struct FamilyRedirect
    {
        const char* askedDirectory;
        const char* askedStem;
        const char* servedDirectory;
        const char* servedStem;
        uint32_t retailRace;
        uint32_t sex;
    };

    // These are not ChrRace table replacements. They are explicit upgrades for custom race rows
    // which deliberately use an old character/creature family. Directory suffix matching accepts
    // the archive's optional HD\ prefix while the output always names the real Retail location.
    constexpr FamilyRedirect kFamilyRedirects[] = {
        // Race 14's male row already asks for the creature-slot HD stem. Route that exact request
        // back to the staged playable character family; its type 1/6/2 texture contract is what the
        // character compositor understands, while the creature model's type 11/12 slots render white.
        { "character\\tuskarr\\",          "tuskarrmale_hd", "character\\tuskarr\\male\\", "tuskarrmale", 0, 0 },
        { "character\\tuskarr\\female\\", "tuskarrfemale", "character\\tuskarr\\", "tuskarrfemale_hd", 0, 1 },
        { "character\\tuskarr\\",          "tuskarrfemale", "character\\tuskarr\\", "tuskarrfemale_hd", 0, 1 },
        // These private ChrRaces rows ask for legacy/custom stems which cannot satisfy the generic
        // "Retail stem extends requested stem" rule below. Resolve only those exact family/stem
        // pairs through their verified Retail ChrModel instead of broad directory substitution.
        // Both faction Dracthyr rows resolve the same dragon model/layout, so the Horde Retail row
        // is a sufficient path oracle for the shared male and female requests.
        // Creation defaults to the distinct visage model. Dragon-form switching is deferred until
        // it has a separate preview owner and cannot re-enter the stock race/sex lifecycle.
        { "character\\dracthyr\\male\\",   "dracthyrmale",   nullptr, nullptr, 76, 0 },
        { "character\\dracthyr\\female\\", "dracthyrfemale", nullptr, nullptr, 76, 1 },
        { "character\\worgen\\male\\",      "worgenmale",      nullptr, nullptr, 22, 0 },
        { "character\\worgen\\female\\",    "worgenfemale",    nullptr, nullptr, 22, 1 },
        // Horde and Alliance Pandaren likewise share their model pair.
        { "character\\pandaren\\male\\",   "pandamale",      nullptr, nullptr, 26, 0 },
        { "character\\pandaren\\female\\", "pandafemale",    nullptr, nullptr, 26, 1 },
        { "character\\goblin\\female\\",   "goblinfm",       nullptr, nullptr, 9,  1 },
        // The active Kul Tiran display rows are stored under Naga_ even though their stems are
        // already correct; an exact redirect is needed because equal stems normally settle early.
        { "character\\naga_\\male\\",        "kultiranmale",   nullptr, nullptr, 32, 0 },
        { "character\\naga_\\female\\",      "kultiranfemale", nullptr, nullptr, 32, 1 },
        { "creature\\murloc\\",            "murloc",        "creature\\murloc2\\",   "murloc2", 0, 0 },
        // Male race 28 deliberately keeps its legacy character actor. Only an explicitly female
        // path may select Maria here; the active shared male path is split by live Glue identity
        // immediately below before these static rules are considered.
        { "character\\murloc\\female\\",  "murlocfemale",  "creature\\murloc2maria\\", "murloc2maria", 0, 1 },
        { "character\\nightelf_dh\\male\\",   "nightelfmale_dh",
          nullptr, nullptr, 85, 0 },
        { "character\\nightelf_dh\\female\\", "nightelffemale_dh",
          nullptr, nullptr, 85, 1 },
        { "character\\bloodelf_dh\\male\\",   "bloodelfmale_dh",
          nullptr, nullptr, 86, 0 },
        { "character\\bloodelf_dh\\female\\", "bloodelffemale_dh",
          nullptr, nullptr, 86, 1 },
    };

    bool RedirectExplicitFamily(const char* name, char* out, uint32_t outCap)
    {
        const char* const file = FileNameOf(name);
        const size_t directoryLength = static_cast<size_t>(file - name);
        const size_t askedStemLength = StemLength(file);

        // Resolve families from the requested path only. Glue's selected sex can
        // still describe the previous model while the next model is being loaded.
        // Redirecting the male Murloc stem based on that stale state mixes Maria's
        // mesh with the male skin/animation family. Female has an explicit rule below.
        for (const FamilyRedirect& rule : kFamilyRedirects)
        {
            const size_t ruleDirectoryLength = std::strlen(rule.askedDirectory);
            const size_t ruleStemLength = std::strlen(rule.askedStem);
            if (directoryLength < ruleDirectoryLength || askedStemLength != ruleStemLength) continue;
            if (!MatchesPathFold(name + directoryLength - ruleDirectoryLength,
                                 rule.askedDirectory, ruleDirectoryLength)) continue;
            if (!MatchesFold(file, rule.askedStem, ruleStemLength)) continue;

            const char* servedDirectory = rule.servedDirectory;
            const char* servedStem = rule.servedStem;
            size_t servedDirectoryLength = servedDirectory ? std::strlen(servedDirectory) : 0;
            size_t servedStemLength = servedStem ? std::strlen(servedStem) : 0;

            // Playable Retail replacements must use the model name resolved from their DB2
            // FileDataID. Guessed friendly paths are not guaranteed to exist in the mounted name
            // catalogue (and Earthen/Harronir consequently fell through to errorcube.m2).
            if (rule.retailRace)
            {
                const WXL_AppearanceApi* const tables = Appearance();
                WXL_Recipe recipe{};
                if (!tables || !tables->BuildForCharacter(rule.retailRace, rule.sex,
                                                           nullptr, 0, &recipe))
                {
                    WLOG_WARN("hd-switch: no Retail recipe for explicit race=%u sex=%u asked=%s",
                              rule.retailRace, rule.sex, name);
                    return false;
                }
                const char* const resolved = wxl_modern_m2::ResolveModel(recipe.modelFileDataId);
                if (!resolved)
                {
                    WLOG_WARN("hd-switch: unresolved model fdid=%u for explicit race=%u sex=%u",
                              recipe.modelFileDataId, rule.retailRace, rule.sex);
                    return false;
                }

                const char* const askedTail = file + askedStemLength;
                if ((std::strlen(askedTail) == 4 && MatchesFold(askedTail, ".mdx", 4))
                    || (std::strlen(askedTail) == 3 && MatchesFold(askedTail, ".m2", 3)))
                {
                    const size_t resolvedLength = std::strlen(resolved);
                    if (resolvedLength + 1 > outCap) return false;
                    std::memcpy(out, resolved, resolvedLength + 1);
                    WLOG_INFO("hd-switch: explicit race=%u sex=%u fdid=%u '%s' -> '%s'",
                              rule.retailRace, rule.sex, recipe.modelFileDataId, name, out);
                    return true;
                }
                servedStem = FileNameOf(resolved);
                servedDirectory = resolved;
                servedDirectoryLength = static_cast<size_t>(servedStem - resolved);
                servedStemLength = StemLength(servedStem);
                if (!servedStemLength) return false;
            }
            const char* tail = file + askedStemLength;
            if (std::strlen(tail) == 4 && MatchesFold(tail, ".mdx", 4)) tail = ".m2";
            const size_t tailLength = std::strlen(tail);
            if (servedDirectoryLength + servedStemLength + tailLength + 1 > outCap) return false;

            std::memcpy(out, servedDirectory, servedDirectoryLength);
            std::memcpy(out + servedDirectoryLength, servedStem, servedStemLength);
            std::memcpy(out + servedDirectoryLength + servedStemLength,
                        tail, tailLength + 1);
            return true;
        }
        return false;
    }

    int __cdecl RedirectToReworked(const char* name, char* out, uint32_t outCap)
    {
        if (!name || !out) return 0;

        if (RedirectExplicitFamily(name, out, outCap)) return 1;

        const char* file = FileNameOf(name);
        const size_t stem = StemLength(file);
        if (!stem) return 0;

        uint32_t race = 0, sex = 0;
        if (!wxl_modern_m2::RaceOfModelPath(name, race, sex)) return 0;
        if (!wxl_modern_m2::RetailCharacterCanaryAllows(race, sex)) return 0;

        const WXL_AppearanceApi* tables = Appearance();
        if (!tables) return 0;

        WXL_Recipe recipe{};
        if (!tables->BuildForCharacter(wxl_modern_m2::RetailCharacterRace(race),
                                       sex, nullptr, 0, &recipe)) return 0;
        const char* const served = wxl_modern_m2::ResolveModel(recipe.modelFileDataId);
        if (!served) return 0;

        const char* const servedFile = FileNameOf(served);
        const size_t servedStem = StemLength(servedFile);
        if (!servedStem) return 0;

        // An equal stem can still name the old HD archive namespace. Canonicalize
        // that exact family once; a request for our canonical output must settle.
        if (servedStem == stem && MatchesFold(file, servedFile, stem))
        {
            const auto canonical = wxl_modern_m2::CanonicalHdAlias(name, served);
            if (canonical.empty() || canonical.size() + 1 > outCap) return 0;
            std::memcpy(out, canonical.c_str(), canonical.size() + 1);
            return 1;
        }

        // And only what the served name is a continuation of. Being under a race's directory is not
        // enough: every one of that race's TEXTURES lives there too, and their stems begin with the
        // model's own -- so a rule that fires on the directory answers a request for a skin file with
        // a name built out of the model's, which is a file nobody shipped. The stock name the tables
        // cannot spell is recoverable this way and only this way: whatever the served model is called,
        // the name it replaces is the part of it the client already asked for.
        if (servedStem <= stem || !MatchesFold(file, servedFile, stem)) return 0;

        // Use the directory the table names. Most stock paths agree with it, but custom race rows can
        // ask below an archive-only HD\ prefix. Keeping that prefix produced a nonexistent path for
        // Draenei female and Blood Elf male and sent both through the error-cube fallback.
        const size_t head = static_cast<size_t>(servedFile - served);
        const size_t tail = std::strlen(file + stem);
        if (head + servedStem + tail + 1 > outCap) return 0;

        std::memcpy(out, served, head);
        std::memcpy(out + head, servedFile, servedStem);
        std::memcpy(out + head + servedStem, file + stem, tail + 1);
        return 1;
    }
}

namespace wxl_modern_m2
{
    bool InstallHdSwitch()
    {
        g_raceFilter = ConfigU32("WXL_M2_RETAIL_CHARACTER_RACE", 0, 0, 255);
        g_sexFilter = ConfigU32("WXL_M2_RETAIL_CHARACTER_SEX", 2, 0, 2);
        const auto* storage = static_cast<const WXL_StorageApi*>(
            g_api->GetInterface("wxl.storage", WXL_STORAGE_API_VERSION));
        if (!storage || storage->structSize < sizeof(WXL_StorageApi) || !storage->RegisterClientRedirect)
        {
            WLOG_WARN("hd-switch: the storage service does not offer name redirects; models stay stock");
            return false;
        }
        storage->RegisterClientRedirect(&RedirectToReworked);
        WLOG_INFO("hd-switch: table-driven race models active (race=%u sex=%u; 0/2 means all)",
                  g_raceFilter, g_sexFilter);
        return true;
    }
}
