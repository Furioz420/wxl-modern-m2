// Server appearance snapshots are owned by GUID, never by a shared model resource.
#include "DragonHornPolicy.hpp"
#include "../ExtensionApi.hpp"
#include "../client/PrivateClientOffsets.hpp"
#include "GilneanPreview.hpp"
#include "game/M2.hpp"
#include <cctype>
#include "engine/events/Event.hpp"
#include "game/Script.hpp"
#include "game/Unit.hpp"
#include "game/World.hpp"
#include "offsets/game/M2.hpp"
#include "wxl/AppearanceApi.h"
#include "wxl/AppearanceCatalogVersion.hpp"
#include "wxl/AppearanceProtocol.hpp"
#include <array>
#include <cmath>
#include <mutex>
#include <unordered_map>
#include <windows.h>
namespace
{
namespace ap = wxl::appearance;
namespace owner = wxl_modern_m2;
namespace script = wxl::game::script;
namespace world = wxl::game::world;
namespace off = wxl::offsets::game::m2;
namespace presentation = wxl_modern_m2::private_offsets::m2;
// Generated from the checked client DB2 snapshot; catalog changes require a matching client build.
constexpr uint32_t CatalogId = ap::ClientCatalogId;
uint32_t nextRequest = 0, ackRequest = 0;
struct Entry
{
    ap::Snapshot data;
    DWORD received = 0;
    void *component = nullptr;
    void *root = nullptr;
    bool applied = false;
    DWORD refreshStarted = 0;
    bool consumed = false;
    bool geometryReady = false;
};
std::unordered_map<uint64_t, Entry> entries;
std::vector<uint64_t> characters;
uint64_t selected = 0;
std::unordered_map<uint64_t, DWORD> queried;
std::unordered_map<uint64_t, DWORD> portraitRefresh;
bool ready = false;
bool worldUi = false;
bool ackPending = false;
uint8_t ackStatus = 255;
std::mutex mutex;
const WXL_AppearanceApi *Tables()
{
    return static_cast<const WXL_AppearanceApi *>(
        owner::g_api->GetInterface("wxl.appearance", WXL_APPEARANCE_API_VERSION));
}
bool Send(uint16_t opcode, const std::vector<uint8_t> &bytes)
{
    return owner::Network()->Send(opcode, bytes.data(), uint32_t(bytes.size())) != 0;
}
void Hello()
{
    std::vector<uint8_t> b;
    ap::Put(b, ap::Version);
    ap::Put(b, CatalogId);
    const bool sent = Send(ap::CmsgHello, b);
    WLOG_INFO("server-appearance: hello protocol=%u catalog=%u sent=%u", unsigned(ap::Version), CatalogId, unsigned(sent));
}
void Query(uint64_t guid)
{
    std::vector<uint8_t> b;
    ap::Put(b, guid);
    Send(ap::CmsgQuery, b);
}
// CurrentMapId describes terrain, which can survive object-manager teardown.
// Build 12340 ActivePlayerGuid (0x4D3790) explicitly returns zero for a null
// TLS object manager; EnumObjects (0x4D4B30) dereferences that same pointer.
bool WorldAppearanceActive()
{
    return worldUi && world::CurrentMapId() >= 0 && world::ActivePlayerGuid() != 0;
}
void* NativeSelectionComponent(bool matchGuid) noexcept
{
    if (worldUi) return nullptr;
    __try
    {
        const int32_t index = *reinterpret_cast<const int32_t*>(presentation::kGlueSelectedCharacter);
        const uint32_t count = *reinterpret_cast<const uint32_t*>(presentation::kGlueCharacterCount);
        auto* records = *reinterpret_cast<uint8_t**>(presentation::kGlueCharacterRecords);
        if (!records || index < 0 || uint32_t(index) >= count) return nullptr;
        if (matchGuid && (!selected || size_t(index) >= characters.size() || characters[index] != selected))
            return nullptr;
        // Native SelectCharacter creates the CMO at 0x4E3DB6 and attaches its
        // +0x38 model to the Glue scene at 0x4E445F. Creation uses a different owner.
        return *reinterpret_cast<void**>(records + size_t(index) * presentation::kGlueCharacterStride + 0x188);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
void* SelectionComponent() noexcept { return NativeSelectionComponent(true); }
void *SafeComponent(uint64_t guid) noexcept
{
    __try
    {
        return wxl::game::unit::CharacterComponent(world::ResolveObject(guid, world::kTypeMaskPlayer));
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return nullptr;
    }
}
void *Root(void *cmo) noexcept
{
    __try
    {
        return cmo ? *reinterpret_cast<void **>(static_cast<uint8_t *>(cmo) + off::kOffCharComponentInstance)
                   : nullptr;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return nullptr;
    }
}
bool selectionHidden = false;
void* hiddenSelectionRoot = nullptr;
float selectionAlphaBase = 1.0f, selectionAlphaStage = 1.0f;
void SetSelectionVisibility(bool visible) noexcept
{
    selectionHidden = !visible;
    auto* root = static_cast<off::M2Instance*>(Root(NativeSelectionComponent(false)));
    if (!root) { if (visible) hiddenSelectionRoot = nullptr; return; }
    __try
    {
        if (!visible)
        {
            if (hiddenSelectionRoot != root)
            {
                hiddenSelectionRoot = root;
                selectionAlphaBase = root->alphaBase;
                selectionAlphaStage = root->alphaStage;
                WLOG_INFO("server-appearance: selection actor hidden root=%p", root);
            }
            root->alphaBase = root->alphaStage = 0.0f;
        }
        else if (hiddenSelectionRoot == root)
        {
            root->alphaBase = std::isfinite(selectionAlphaBase) && selectionAlphaBase > 0.0f ? selectionAlphaBase : 1.0f;
            root->alphaStage = std::isfinite(selectionAlphaStage) && selectionAlphaStage > 0.0f ? selectionAlphaStage : 1.0f;
            hiddenSelectionRoot = nullptr;
            WLOG_INFO("server-appearance: selection actor revealed root=%p", root);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}
int __cdecl SelectionVisibility(void* state)
{
    SetSelectionVisibility(script::ToNumber(state, 1) != 0);
    return 0;
}
void __cdecl SelectionPerFrame(void*, const void* raw)
{
    if (!selectionHidden || worldUi || !raw) return;
    const auto& args = *static_cast<const wxl::events::M2PerFrameUpdateArgs*>(raw);
    if (args.renderCtx == Root(NativeSelectionComponent(false))) SetSelectionVisibility(false);
}
bool Identity(void *cmo, const ap::Selection &s) noexcept
{
    __try
    {
        auto *p = static_cast<uint8_t *>(cmo);
        return p && *reinterpret_cast<uint32_t *>(p + off::kOffCharComponentRace) == s.race &&
               *reinterpret_cast<uint32_t *>(p + off::kOffCharComponentSex) == s.sex;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}
const char* ModelStem(void* cmo) noexcept
{
    __try {
        auto* root = Root(cmo);
        auto* model = root ? *reinterpret_cast<void**>(static_cast<uint8_t*>(root) + off::kOffInstShared) : nullptr;
        return model ? wxl::game::m2::M2Model(model).GetPathStem() : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
std::string ModelKey(const char* path)
{
    std::string key = path ? path : "";
    for (auto& c : key) c = c == '/' ? '\\' : char(std::tolower(static_cast<unsigned char>(c)));
    auto dot = key.find_last_of('.'); if (dot != std::string::npos) key.resize(dot);
    return key;
}
bool AlternateMatches(void* cmo, const ap::Selection& s)
{
    auto* t = Tables();
    const auto retail = ap::AlternateRetailRace(s.race);
    if (!retail || !t || !cmo || s.alternate.empty()) return false;
    WXL_Recipe recipe{};
    if (!t->BuildForCharacter(retail, s.sex, nullptr, 0, &recipe)) return false;
    auto expected = ModelKey(owner::ResolveModel(recipe.modelFileDataId));
    return !expected.empty() && expected == ModelKey(ModelStem(cmo));
}
bool Matches(void* cmo, const ap::Selection& s)
{
    return Identity(cmo,s) || AlternateMatches(cmo,s);
}
void Dirty(void *cmo) noexcept
{
    __try
    {
        if (cmo)
            *(static_cast<uint8_t *>(cmo) + off::kOffCharComponentRebuild) |=
                off::kCharRebuildSheet | off::kCharRebuildGeosets;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}
// Character selection can leave a static model out of the native update queue.
// Drive its pending work on the game thread after releasing the snapshot mutex:
// composition calls ServerAppearanceChoices again and must be able to acquire it.
void RefreshSelected(void* cmo) noexcept
{
    __try
    {
        const auto pending = *reinterpret_cast<const uint8_t*>(static_cast<uint8_t*>(cmo) + off::kOffCharComponentRebuild);
        if (!(pending & (off::kCharRebuildSheet | off::kCharRebuildGeosets))) return;
        // Build 12340: 0x004F1520 is thiscall with one argument, all exits RET 4.
        using RefreshFn = bool(__thiscall*)(void*, uint32_t);
        reinterpret_cast<RefreshFn>(off::kCharRenderPrep)(cmo, 1);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        WLOG_ERROR("server-appearance: selected model refresh failed");
    }
}
void InvalidatePortrait(uint64_t guid) noexcept
{
    if (!WorldAppearanceActive()) return;
    __try
    {
        // SetPortraitTexture reuses a clean GUID cache entry. The native
        // invalidator marks only this player's entry dirty, retaining engine
        // ownership of its resources and notifying existing portrait widgets.
        wxl::game::Native<presentation::InvalidatePlayerPortraitFn>(
            presentation::kInvalidatePlayerPortrait)(&guid, 2u);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        WLOG_ERROR("server-appearance: portrait cache invalidation failed");
    }
}
bool ValidatePrimary(const ap::Selection &s)
{
    auto *t = Tables();
    if (!t || s.catalog != CatalogId)
        return false;
    if (ap::PrivateModel(s.race, s.sex))
    {
        if (s.race == 23)
        {
            if (s.choices.size() != (s.sex == 1 ? 2u : 1u)) return false;
            bool hair = false, horn = s.sex == 0;
            for (auto c : s.choices) {
                if (c.option == ap::PrivateHairColor && c.choice >= 1 && c.choice <= 10) hair = true;
                else if (s.sex == 1 && c.option == ap::PrivateHornStyle && c.choice >= 1 && c.choice <= 3) horn = true;
                else return false;
            }
            return hair && horn;
        }
        if (s.choices.size() != (s.race == 14 ? 1u : 2u))
            return false;
        for (auto choice : s.choices)
            if ((choice.option != ap::PrivateSkin && (s.race != 28 || choice.option != ap::PrivateOutfit)) ||
                !choice.choice || choice.choice > (s.race == 14 ? 3u : 5u))
                return false;
        return true;
    }
    uint32_t model = t->ChrModelForRace(owner::RetailCharacterRace(s.race), s.sex);
    if (!model || t->OptionCount(model) != s.choices.size())
        return false;
    for (auto pair : s.choices)
    {
        bool found = false;
        for (uint32_t i = 0; i < t->OptionCount(model); ++i)
        {
            WXL_ChrOption o{};
            if (!t->OptionAt(model, i, &o) || o.id != pair.option)
                continue;
            for (uint32_t j = 0; j < t->ChoiceCount(o.id); ++j)
            {
                WXL_ChrChoice c{};
                if (t->ChoiceAt(o.id, j, &c) && c.id == pair.choice)
                {
                    found = true;
                    break;
                }
            }
            break;
        }
        if (!found)
            return false;
    }
    return true;
}
bool Validate(const ap::Selection& s)
{
    auto copy = s;
    if (!ap::Canonical(copy) || !ValidatePrimary(copy)) return false;
    auto retail = ap::AlternateRetailRace(s.race);
    if (!retail) return s.alternate.empty();
    auto* t = Tables();
    if (!t || s.alternate.size() != owner::AlternateOptionCount(retail,s.sex)) return false;
    for (uint32_t i = 0; i < s.alternate.size(); ++i) {
        WXL_ChrOption o{};
        if (!owner::AlternateOptionAt(retail,s.sex,i,&o)) return false;
        auto chosen = std::find_if(s.alternate.begin(),s.alternate.end(),[&](auto c){return c.option==o.id;});
        if (chosen == s.alternate.end()) return false;
        bool found = false;
        for (uint32_t j=0;j<t->ChoiceCount(o.id);++j) {
            WXL_ChrChoice c{};
            if (t->ChoiceAt(o.id,j,&c) && c.id == chosen->choice) {found=true;break;}
        }
        if (!found) return false;
    }
    return true;
}
void __cdecl Snapshot(const uint8_t *p, uint32_t n, void *)
{
    ap::Snapshot s;
    if (!ap::DecodeSnapshot(p, n, s) || s.selection.catalog != CatalogId || s.guid > UINT32_MAX)
        return;
    std::lock_guard lock(mutex);
    if (!ready)
        return;
    auto i = entries.find(s.guid);
    if (i == entries.end() && entries.size() >= 512)
        return;
    if (i != entries.end() && i->second.data.revision > s.revision)
        return;
    WLOG_INFO("server-appearance: snapshot guid=%llu revision=%u race=%u sex=%u choices=%u",
              static_cast<unsigned long long>(s.guid), s.revision, unsigned(s.selection.race), unsigned(s.selection.sex), unsigned(s.selection.choices.size()));
    entries[s.guid] = {std::move(s), GetTickCount(), nullptr, nullptr, false};
}
void __cdecl HelloReply(const uint8_t *p, uint32_t n, void *)
{
    ap::Reader r(p, n);
    uint8_t v = 0, ok = 0;
    uint32_t id = 0, map = 0;
    if (!r.Read(v) || !r.Read(id) || !r.Read(map) || !r.Read(ok) || r.Remaining())
        return;
    {
        std::lock_guard lock(mutex);
        ready = v == ap::Version && id == CatalogId && map == ap::RaceMapVersion && ok == 1;
    }
    WLOG_INFO("server-appearance: hello reply protocol=%u catalog=%u map=%u enabled=%u ready=%u",
              unsigned(v), id, map, unsigned(ok), unsigned(ready));
    if (ready)
    {
        for (auto guid : characters)
            Query(guid);
        if (WorldAppearanceActive())
            Query(world::ActivePlayerGuid());
    }
}
void __cdecl Prepared(const uint8_t *p, uint32_t n, void *)
{
    ap::Reader r(p, n);
    uint32_t id = 0;
    uint8_t status = 255;
    if (!r.Read(id) || !r.Read(status) || r.Remaining())
        return;
    std::lock_guard lock(mutex);
    WLOG_INFO("server-appearance: prepared request=%u status=%u", id, unsigned(status));
    ackPending = true;
    ackStatus = status;
    ackRequest = id;
}
void __cdecl Enum(const uint8_t *p, uint32_t n, void *)
{
    // Existing 3.3.5 enumeration layout: 23 equipment/bag slots, 9 bytes each.
    ap::Reader r(p, n);
    uint8_t count = 0;
    if (!r.Read(count) || count > 32)
        return;
    std::vector<uint64_t> ids;
    for (uint8_t i = 0; i < count; ++i)
    {
        uint64_t guid = 0;
        uint8_t c = 0;
        if (!r.Read(guid))
            return;
        unsigned name = 0;
        do
        {
            if (!r.Read(c) || ++name > 49)
                return;
        } while (c);
        // race/class/sex + five appearance bytes + level; zone/map/position/guild/flags/customize;
        // first login; pet display/level/family; 23 visible slots.
        for (unsigned j = 0; j < 9 + 20 + 12 + 1 + 12 + 23 * 9; ++j)
            if (!r.Read(c))
                return;
        ids.push_back(guid);
    }
    if (r.Remaining())
        return;
    {
        std::lock_guard lock(mutex);
        worldUi = false;
        characters = std::move(ids);
        selected = 0;
        ready = false;
        entries.clear();
        queried.clear();
        portraitRefresh.clear();
    }
    WLOG_INFO("server-appearance: character list parsed count=%u", unsigned(count));
    Hello();
}
int __cdecl Prepare(void *state)
{
    size_t length = 0;
    const char *name = script::ToString(state, 1, &length);
    uint32_t race = 0, sex = 0;
    ap::Selection s;
    s.catalog = CatalogId;
    if (!name || !length || length > 48 || !owner::CustomizeIdentity(race, sex))
    {
        script::PushNumber(state, -1);
        return 1;
    }
    auto *t = Tables();
    if (!t)
    {
        WLOG_WARN("server-appearance: prepare failed: appearance tables unavailable");
        script::PushNumber(state, -2);
        return 1;
    }
    uint32_t model = t->ChrModelForRace(owner::RetailCharacterRace(race), sex);
    bool synthetic = ap::PrivateModel(uint8_t(race), uint8_t(sex)) != 0;
    if (!synthetic && (!model || !owner::RetailCharacterCanaryAllows(race, sex)))
    {
        script::PushNumber(state, 0);
        return 1;
    }
    s.race = uint8_t(race);
    s.sex = uint8_t(sex);
    std::array<uint32_t, ap::MaxChoices> settled{};
    uint32_t count = owner::SettledChoicesFor(owner::CustomizeComponent(), model, settled.data(),
                                              uint32_t(settled.size()));
    if (!ready || (!synthetic && count != t->OptionCount(model)))
    {
        WLOG_WARN("server-appearance: prepare blocked ready=%u race=%u sex=%u model=%u settled=%u expected=%u",
                  unsigned(ready), race, sex, model, count, t->OptionCount(model));
        script::PushNumber(state, !ready ? -3 : -4);
        return 1;
    }
    if (synthetic && race == 23) {
        s.choices.push_back({ap::PrivateHairColor, owner::LegacyBrokenHairColor() + 1});
        if (sex == 1) s.choices.push_back({ap::PrivateHornStyle, owner::LegacyBrokenHornStyle() + 1});
    }
    else if (synthetic)
    {
        uint32_t skin = race == 28
                            ? owner::MurlocFemaleSkin()
                            : uint32_t(std::max(0, owner::CustomizeChoiceFor(t->ChrModelForRace(17, 0), 0)));
        s.choices.push_back({ap::PrivateSkin, skin + 1});
        if (race == 28)
            s.choices.push_back({ap::PrivateOutfit, owner::MurlocFemaleOutfit() + 1});
    }
    else
        for (uint32_t i = 0; i < t->OptionCount(model); ++i)
        {
            WXL_ChrOption o{};
            if (!t->OptionAt(model, i, &o))
                break;
            bool found = false;
            for (uint32_t j = 0; j < t->ChoiceCount(o.id); ++j)
            {
                WXL_ChrChoice c{};
                if (!t->ChoiceAt(o.id, j, &c))
                    continue;
                if (std::find(settled.begin(), settled.begin() + count, c.id) != settled.begin() + count)
                {
                    s.choices.push_back({o.id, c.id});
                    found = true;
                    break;
                }
            }
            if (!found)
                break;
        }
    if (ap::AlternateRetailRace(s.race)) {
        std::array<uint32_t, ap::MaxChoices> alternate{};
        uint32_t n = owner::GilneanChoices(sex, alternate.data(), uint32_t(alternate.size()));
        for (uint32_t i=0;i<n;++i) {
            WXL_ChrOption o{};
            if (!owner::AlternateOptionAt(ap::AlternateRetailRace(s.race),sex,i,&o)) break;
            s.alternate.push_back({o.id,alternate[i]});
        }
    }
    if (!ap::Canonical(s) || !Validate(s))
    {
        WLOG_WARN("server-appearance: prepare invalid race=%u sex=%u model=%u choices=%u", race, sex, model, unsigned(s.choices.size()));
        script::PushNumber(state, -5);
        return 1;
    }
    uint32_t request = ++nextRequest;
    if (!request)
        request = ++nextRequest;
    std::vector<uint8_t> b;
    ap::Put(b, ap::Version);
    ap::Put(b, request);
    ap::PutSelection(b, s);
    b.insert(b.end(), name, name + length);
    b.push_back(0);
    const bool sent = Send(ap::CmsgPrepare, b);
    WLOG_INFO("server-appearance: prepare request=%u race=%u sex=%u choices=%u sent=%u", request, race, sex, unsigned(s.choices.size()), unsigned(sent));
    script::PushNumber(state, sent ? double(request) : -6.0);
    return 1;
}
// UI session state is separate from terrain and the resident object manager:
// both may survive logout while character selection is already visible.
int __cdecl SetContext(void* state)
{
    const bool active = script::ToNumber(state, 1) == 1;
    std::lock_guard lock(mutex);
    if (worldUi != active)
    {
        // Restore the selected actor while the Glue record is still addressable.
        if (active) SetSelectionVisibility(true);
        worldUi = active;
        selected = 0;
        portraitRefresh.clear();
        for (auto& pair : entries)
        {
            auto& entry = pair.second;
            entry.component = entry.root = nullptr;
            entry.applied = entry.consumed = entry.geometryReady = false;
        }
        WLOG_INFO("server-appearance: UI context=%s", active ? "world" : "glue");
    }
    return 0;
}
bool SheetPending(void* cmo) noexcept
{
    if (!cmo) return true;
    __try
    {
        return (*reinterpret_cast<const uint8_t*>(static_cast<uint8_t*>(cmo) + off::kOffCharComponentRebuild) &
                (off::kCharRebuildSheet | off::kCharRebuildGeosets)) != 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return true; }
}
int __cdecl SelectionStatus(void* state)
{
    int status = 0; // no modern snapshot: use this character's normal legacy appearance
    {
        std::lock_guard lock(mutex);
        if (!worldUi && selected)
        {
            const auto it = entries.find(selected);
            if (!ready) status = 1;
            else if (it != entries.end())
            {
                const auto& entry = it->second;
                void* const cmo = SelectionComponent();
                const bool modern = !ap::PrivateModel(entry.data.selection.race, entry.data.selection.sex);
                status = (!entry.applied || !entry.consumed || (modern && !entry.geometryReady) || entry.component != cmo ||
                          entry.root != Root(cmo) || SheetPending(cmo) ||
                          (modern && !owner::CustomizationAttachmentsReady(cmo))) ? 1 : 0;
            }
        }
    }
    script::PushNumber(state, status);
    return 1;
}
int __cdecl Select(void *state)
{
    double d = script::ToNumber(state, 1);
    uint64_t guid = 0;
    {
        std::lock_guard lock(mutex);
        if (std::isfinite(d) && d >= 1 && d <= characters.size() && std::floor(d) == d)
            guid = characters[size_t(d) - 1];
        if (selected != guid)
        {
            selected = guid;
            WLOG_INFO("server-appearance: selected listIndex=%u guid=%llu", unsigned(d >= 0 && d <= UINT32_MAX ? d : 0), static_cast<unsigned long long>(guid));
            for (auto &pair : entries)
                pair.second.applied = false;
        }
    }
    return 0;
}
void __cdecl Update(void *, const void *)
{
    // Glue and FrameXML may reuse a Lua-state address while discarding frames.
    // Do not depend on the bootstrap frame surviving to switch recipe ownership.
    // Run before taking mutex: the registered context callback acquires it.
    static uint32_t contextChecked = 0;
    const uint32_t contextNow = GetTickCount();
    if (contextNow - contextChecked >= 200)
    {
        contextChecked = contextNow;
        script::Execute("if type(IsLoggedIn)=='function' and type(_WXL_APPEARANCE_CONTEXT)=='function' then _WXL_APPEARANCE_CONTEXT(IsLoggedIn() and 1 or 0) end",
                        "WXLAppearanceContextRefresh");
    }

    bool dispatch = false;
    uint8_t status = 255;
    uint32_t request = 0;
    {
        std::lock_guard lock(mutex);
        dispatch = ackPending;
        status = ackStatus;
        request = ackRequest;
        ackPending = false;
    }
    if (dispatch)
    {
        std::string code = "if WXLAppearanceCreateReady then WXLAppearanceCreateReady(" +
                           std::to_string(request) + "," + (status == 0 ? "true" : "false") + ") end";
        script::Execute(code.c_str(), "WXLAppearanceAck");
    }
    static DWORD rescan = 0;
    DWORD now = GetTickCount();
    if (ready && WorldAppearanceActive() && now - rescan >= 1000)
    {
        rescan = now;
        unsigned sent = 0;
        world::ForEachObject([&](uint64_t guid) {
            if (!guid || guid > UINT32_MAX)
                return true;
            bool request = false;
            {
                std::lock_guard lock(mutex);
                auto old = queried.find(guid);
                request = entries.find(guid) == entries.end() &&
                          (old == queried.end() || now - old->second >= 10000);
                if (request)
                {
                    if (queried.size() >= 512 && old == queried.end())
                        queried.erase(queried.begin());
                    queried[guid] = now;
                }
            }
            if (request)
            {
                Query(guid);
                ++sent;
            }
            return sent < 24;
        });
    }
    static DWORD last = 0;
    if (now - last < 100)
        return;
    last = now;
    struct GeometryWork { uint64_t guid; void* component; void* root; };
    std::vector<void*> refresh;
    std::vector<GeometryWork> geometry;
    std::vector<uint64_t> portraits;
    {
    std::lock_guard lock(mutex);
    bool worldContext = WorldAppearanceActive();
    for (auto i = entries.begin(); i != entries.end();)
    {
        auto &e = i->second;
        void *cmo = worldContext ? SafeComponent(i->first)
                                 : (selected == i->first ? SelectionComponent() : nullptr);
        if (!cmo)
        {
            if (worldContext && (e.component || now - e.received > 30000))
            {
                i = entries.erase(i);
                continue;
            }
            ++i;
            continue;
        }
        void *root = Root(cmo);
        if (!root || !Matches(cmo, e.data.selection))
        {
            e.applied = false;
            ++i;
            continue;
        }
        if (!e.applied || e.component != cmo || e.root != root)
        {
            if (!Validate(e.data.selection))
            {
                i = entries.erase(i);
                continue;
            }
            e.component = cmo;
            e.root = root;
            e.applied = true;
            e.refreshStarted = now;
            e.consumed = false;
            e.geometryReady = false;
            WLOG_INFO("server-appearance: apply guid=%llu context=%s choices=%u",
                      static_cast<unsigned long long>(i->first), worldContext ? "world" : "select", unsigned(e.data.selection.choices.size()));
            Dirty(cmo);
        }
        if ((now - e.refreshStarted < 5000u || (!worldContext && !e.geometryReady)) && refresh.size() < 4)
        {
            refresh.push_back(cmo);
            if (!e.geometryReady) geometry.push_back({i->first, cmo, root});
        }
        ++i;
    }
    if (worldContext)
        for (auto i = portraitRefresh.begin(); i != portraitRefresh.end();)
        {
            if (int32_t(now - i->second) >= 0 && portraits.size() < 4)
            {
                portraits.push_back(i->first);
                i = portraitRefresh.erase(i);
            }
            else ++i;
        }
    }
    for (void* cmo : refresh) RefreshSelected(cmo);
    for (const auto& work : geometry)
    {
        // This pass resolves choices through this same snapshot mutex.
        if (!owner::RefreshAppearanceGeometry(work.component)) continue;
        std::lock_guard lock(mutex);
        auto found = entries.find(work.guid);
        if (found != entries.end() && found->second.component == work.component &&
            found->second.root == work.root && Root(work.component) == work.root)
        {
            found->second.geometryReady = true;
            WLOG_INFO("server-appearance: geometry restored guid=%llu cmo=%p root=%p",
                      static_cast<unsigned long long>(work.guid), work.component, work.root);
            if (WorldAppearanceActive()) portraitRefresh[work.guid] = now + 200;
        }
    }
    for (uint64_t guid : portraits)
    {
        InvalidatePortrait(guid);
        WLOG_INFO("server-appearance: portrait refresh guid=%llu", static_cast<unsigned long long>(guid));
        const std::string code = "if WXLAppearanceRefreshPortraits then WXLAppearanceRefreshPortraits(" + std::to_string(guid) + ") end";
        script::Execute(code.c_str(), "WXLAppearancePortrait");
    }
}
void __cdecl Leave(void *, const void *)
{
    std::lock_guard lock(mutex);
    // A map transfer replaces model instances, not character-owned recipes.
    // A new character enumeration clears this cache at the account/session boundary.
    for (auto& pair : entries)
    {
        auto& e = pair.second;
        e.component = e.root = nullptr;
        e.applied = e.consumed = e.geometryReady = false;
        e.received = GetTickCount();
    }
    queried.clear();
    portraitRefresh.clear();
    selected = 0;
    WLOG_INFO("server-appearance: world leave retained %u recipes; instance bindings cleared", unsigned(entries.size()));
}
void __cdecl Enter(void *, const void *)
{
    if (ready && WorldAppearanceActive())
        Query(world::ActivePlayerGuid());
}
const char *Lua = R"lua(
if not WXLAppearanceBridge then
 WXLAppearanceBridge={}; local b=WXLAppearanceBridge
 local function showError(text)
  b.lastError=text
  if GlueDialog and type(GlueDialog.ShowDialog)=="function" then
   GlueDialog:ShowDialog("OKAY",text)
  elseif type(GlueDialog_Show)=="function" then
   GlueDialog_Show("OKAY",text)
  elseif type(print)=="function" then print(text) end
 end
 local errors={
  [-1]="The character name or appearance is not ready. Please try again.",
  [-2]="Character appearance data is unavailable. Please restart the client.",
  [-3]="Appearance saving is not connected to the server. Check that the server has WXL appearance enabled with the matching catalog, then reconnect.",
  [-4]="The character model is still updating. Please wait for it to finish and try again.",
  [-5]="The selected appearance does not match the client appearance catalog. Please check the client update.",
  [-6]="The appearance could not be sent to the server. Please reconnect and try again."
 }
 -- Portrait textures are snapshots of a separate model. Track each unit so
 -- late appearance/attachment completion refreshes only the matching player.
 b.portraitUnits=setmetatable({}, {__mode="k"})
 -- FrameXML installs this native function after some bootstrap scripts run.
 -- Capture it when available, not permanently as nil during Glue startup.
 local function ensurePortraitHook()
  local native=SetPortraitTexture
  if type(native)=="function" and native~=b.portraitHook then
   b.portraitNative=native
   b.portraitHook=function(texture,unit,...)
    if texture and type(unit)=="string" then b.portraitUnits[texture]=unit end
    return native(texture,unit,...)
   end
   SetPortraitTexture=b.portraitHook
  end
 end
 ensurePortraitHook()
 WXLAppearanceRefreshPortraits=function(guid)
  ensurePortraitHook()
  local portrait=b.portraitNative
  if type(portrait)~="function" or type(UnitGUID)~="function" then return end
  local seen={}
  local function refresh(texture,unit)
   if not texture or seen[texture] then return end
   local current=UnitGUID(unit)
   if current and tonumber(current)==guid then
    seen[texture]=true;portrait(texture,unit)
   end
  end
  for texture,unit in pairs(b.portraitUnits) do refresh(texture,unit) end
  -- These may have painted before the tracking hook was installed.
  refresh(PlayerPortrait,"player")
  refresh(PlayerFrame and PlayerFrame.portrait,"player")
  refresh(CharacterFramePortrait,"player")
  refresh(CharacterFrame and CharacterFrame.portrait,"player")
  local player=UnitGUID("player")
  if player and tonumber(player)==guid and CharacterModelFrame and
     CharacterModelFrame.IsShown and CharacterModelFrame:IsShown() and
     CharacterModelFrame.SetUnit then CharacterModelFrame:SetUnit("player") end
 end
 local function selectionScreenActive()
  local root=CharacterSelect
  if not root or not root.IsShown or not root:IsShown() then return false end
  if type(IsLoggedIn)=="function" and IsLoggedIn() then return false end
  if type(IsConnectedToServer)~="function" or IsConnectedToServer()~=1 then return false end
  if type(GetNumCharacters)~="function" or (GetNumCharacters() or 0)<1 then return false end
  if GlueDialog and GlueDialog.IsShown and GlueDialog:IsShown() then return false end
  return true
 end
 local function selectionLoading(loading)
  local root=CharacterSelect
  if not selectionScreenActive() then loading=false end
  if loading and not b.selectionSpinner then
   local spinner=CreateFrame("Frame","WXLSelectionLoadingSpinner",root,"LoadingSpinnerTemplate")
   spinner:SetSize(48,48)
   -- The name follows the scene actor even when the Glue viewport is offset.
   spinner:SetPoint("CENTER",CharSelectCharacterName or root,"CENTER",0,root:GetHeight()*0.45)
   spinner:SetFrameStrata("FULLSCREEN_DIALOG");spinner:SetFrameLevel(30);spinner:EnableMouse(false)
   b.selectionSpinner=spinner;spinner:Hide()
   root:HookScript("OnHide",function() selectionLoading(false) end)
  end
  if b.selectionHidden~=loading then
   _WXL_APPEARANCE_SELECTION_VISIBLE(loading and 0 or 1)
   b.selectionHidden=loading
  end
  local spinner=b.selectionSpinner
  if spinner then
   local anim=spinner.AnimFrame and spinner.AnimFrame.Anim
   if loading then
    if not spinner:IsShown() then spinner:Show();if anim then anim:Play() end end
   else spinner:Hide();if anim then anim:Stop() end end
  end
 end
 -- End loading visibility before native world entry can reuse the selected actor.
 -- OnHide can arrive after the world context has made Glue records inaccessible.
 local nativeEnterWorld=EnterWorld
 if type(nativeEnterWorld)=="function" then
  EnterWorld=function(...)
   selectionLoading(false)
   _WXL_APPEARANCE_SELECTION_VISIBLE(1)
   b.selectionHidden=false
   return nativeEnterWorld(...)
  end
 end
 local nativeSelect=SelectCharacter
 if type(nativeSelect)=="function" then
  SelectCharacter=function(...)
   -- Give the newly selected actor an immediate loading state. Snapshot delivery
   -- can trail the character-list response; allow that short grace before reveal.
   b.selectionAge=0;b.selectionStable=0;b.selectionTimedOut=nil
   selectionLoading(false)
   -- SelectCharacter takes the native 1-based list index, already translated by
   -- the UI. Bind its GUID BEFORE native composition reads the appearance.
   _WXL_APPEARANCE_SELECT(select(1,...) or 0)
   local result=nativeSelect(...)
   b.selectionHidden=false
   selectionLoading(true)
   return result
  end
 end
 local original=CreateCharacter
 if type(original)=="function" then
  CreateCharacter=function(name)
   if b.pending then return end
   local status=_WXL_APPEARANCE_PREPARE(name)
   if status==0 then return original(name) end
   if status>0 then b.pending=name;b.request=status;b.age=0;return end
   showError(errors[status] or "Character appearance could not be saved. Please try again.")
  end
  WXLAppearanceCreateReady=function(request,ok)
   if b.request~=request then return end
   local name=b.pending;b.pending=nil;b.request=nil
   if not name then return end
   if ok then original(name) else showError("The server rejected this appearance selection.") end
  end
 end
 b.frame=CreateFrame("Frame");b.frame:SetScript("OnUpdate",function(_,dt)
  b.elapsed=(b.elapsed or 0)+dt;if b.pending then b.age=b.age+dt;if b.age>10 then b.pending=nil;b.request=nil;showError("Appearance save timed out. Please try again.") end end
  if b.elapsed<0.2 then return end;b.elapsed=0
  _WXL_APPEARANCE_CONTEXT(type(IsLoggedIn)=="function" and IsLoggedIn() and 1 or 0)
  ensurePortraitHook()
  if selectionScreenActive() then
   local index=CharacterSelect.selectedIndex
   if index and type(GetCharIDFromIndex)=="function" then index=GetCharIDFromIndex(index)
   elseif type(GetSelectedCharacterIndex)=="function" then index=GetSelectedCharacterIndex() end
   _WXL_APPEARANCE_SELECT(index or 0)
   if b.lastSelection~=index then
    b.lastSelection=index;b.selectionAge=0;b.selectionStable=0;b.selectionTimedOut=nil;b.selectionHidden=false
   end
   b.selectionAge=(b.selectionAge or 0)+0.2
   local pending=_WXL_APPEARANCE_SELECTION_STATUS()~=0
   b.selectionStable=pending and 0 or (b.selectionStable or 0)+0.2
   local loading=b.selectionAge<1 or pending or b.selectionStable<0.4
   -- Slow or incomplete appearances stay hidden. Navigation remains available.
   if loading and b.selectionAge>20 then
    if not b.selectionTimedOut then
     b.selectionTimedOut=true
     if type(print)=="function" then print("Character appearance is still loading.") end
    end
   end
   selectionLoading(loading)
  else
   _WXL_APPEARANCE_SELECT(0);selectionLoading(false)
   b.selectionAge=0;b.selectionStable=0;b.selectionTimedOut=nil
  end
 end)
end
)lua";
} // namespace
namespace wxl_modern_m2
{
void NotifyAppearanceVisualsChanged(void* cmo)
{
    if (!cmo || !WorldAppearanceActive()) return;
    std::lock_guard lock(mutex);
    if (!ready) return;
    for (const auto& pair : entries)
        if (SafeComponent(pair.first) == cmo && Matches(cmo, pair.second.data.selection))
            portraitRefresh[pair.first] = GetTickCount() + 200;
}
bool ServerAlternateAppearanceIdentity(const void* cmo, uint32_t& retailRace, uint32_t& sex)
{
    if (!cmo || !WorldAppearanceActive()) return false;
    std::lock_guard lock(mutex);
    if (!ready) return false;
    for (auto& [guid,e] : entries)
        if (SafeComponent(guid) == cmo && AlternateMatches(const_cast<void*>(cmo),e.data.selection)) {
            retailRace = ap::AlternateRetailRace(e.data.selection.race);
            sex = e.data.selection.sex;
            return true;
        }
    return false;
}
bool ServerAppearanceChoices(void *cmo, uint32_t model, uint32_t *out, uint32_t capacity, uint32_t &count)
{
    count = 0;
    std::lock_guard lock(mutex);
    if (!ready || !cmo)
        return false;
    for (auto &pair : entries)
    {
        auto &e = pair.second;
        const bool inWorld = WorldAppearanceActive();
        // Resolve GUID ownership afresh for both world and selection. The native
        // record owns a newly created CMO before Update has recorded e.root; waiting
        // for that binding paints a default atlas on the first selection build.
        bool matches = inWorld ? SafeComponent(pair.first) == cmo
                               : selected == pair.first && SelectionComponent() == cmo;
        if (!matches || !Matches(cmo, e.data.selection) || !Root(cmo))
            continue;
        auto *t = Tables();
        uint32_t expected = ap::PrivateModel(e.data.selection.race, e.data.selection.sex);
        if (!expected)
            expected =
                t ? t->ChrModelForRace(RetailCharacterRace(e.data.selection.race), e.data.selection.sex) : 0;
        const auto* choices = &e.data.selection.choices;
        if (AlternateMatches(cmo,e.data.selection)) {
            expected = t ? t->ChrModelForRace(ap::AlternateRetailRace(e.data.selection.race),e.data.selection.sex) : 0;
            choices = &e.data.selection.alternate;
        }
        if (expected != model || choices->size() > capacity) return false;
        for (auto choice : *choices)
            out[count++] = dragon::RequiredHorns(choice.choice);
        if (!e.consumed)
        {
            e.consumed = true;
            if (inWorld) portraitRefresh[pair.first] = GetTickCount() + 200;
            WLOG_INFO("server-appearance: renderer consumed guid=%llu model=%u choices=%u",
                      static_cast<unsigned long long>(pair.first), model, count);
            for (auto choice : e.data.selection.choices)
                if (choice.option == 1788 || choice.option == 1823)
                    WLOG_INFO("server-appearance: saved scale color guid=%llu option=%u choice=%u",
                              static_cast<unsigned long long>(pair.first), choice.option, choice.choice);
        }
        return true;
    }
    return false;
}
int ServerPrivateAppearanceChoice(void *cmo, uint32_t race, uint32_t sex, uint32_t option)
{
    std::lock_guard lock(mutex);
    if (!ready || !cmo)
        return -1;
    for (auto &pair : entries)
    {
        auto &e = pair.second;
        const bool inWorld = WorldAppearanceActive();
        // Resolve GUID ownership afresh for both world and selection. The native
        // record owns a newly created CMO before Update has recorded e.root; waiting
        // for that binding paints a default atlas on the first selection build.
        bool matches = inWorld ? SafeComponent(pair.first) == cmo
                               : selected == pair.first && SelectionComponent() == cmo;
        if (!matches || !Matches(cmo, e.data.selection) || !Root(cmo) ||
            e.data.selection.race != race || e.data.selection.sex != sex)
            continue;
        for (auto choice : e.data.selection.choices)
            if (choice.option == option)
                return int(choice.choice) - 1;
    }
    return -1;
}
int ServerAppearanceChoice(void *cmo, uint32_t model, uint32_t option)
{
    std::array<uint32_t, ap::MaxChoices> ids{};
    uint32_t count = 0;
    if (!ServerAppearanceChoices(cmo, model, ids.data(), uint32_t(ids.size()), count))
        return -1;
    auto *t = Tables();
    for (uint32_t j = 0; t && j < t->ChoiceCount(option); ++j)
    {
        WXL_ChrChoice c{};
        if (t->ChoiceAt(option, j, &c) &&
            std::find(ids.begin(), ids.begin() + count, c.id) != ids.begin() + count)
            return int(j);
    }
    return -1;
}
bool InstallServerAppearance()
{
    if (!ConfigBool("WXL_M2_SERVER_APPEARANCE", false))
        return true;
    auto *n = Network();
    auto *f = FrameScript();
    if (!n || !f)
        return false;
    bool ok = n->RegisterClientOpcode(ap::CmsgHello, "CMSG_WXL_APPEARANCE_HELLO") &&
              n->RegisterClientOpcode(ap::CmsgPrepare, "CMSG_WXL_APPEARANCE_PREPARE") &&
              n->RegisterClientOpcode(ap::CmsgQuery, "CMSG_WXL_APPEARANCE_QUERY");
    ok &= n->RegisterServerOpcode(ap::SmsgHello, "SMSG_WXL_APPEARANCE_HELLO", HelloReply, nullptr) != 0;
    ok &= n->RegisterServerOpcode(ap::SmsgSnapshot, "SMSG_WXL_APPEARANCE_SNAPSHOT", Snapshot, nullptr) != 0;
    ok &= n->RegisterServerOpcode(ap::SmsgPrepared, "SMSG_WXL_APPEARANCE_PREPARED", Prepared, nullptr) != 0;
    ok &= n->RegisterServerObserver(0x3B, "WXL appearance character list", Enum, nullptr) != 0;
    ok &= f->RegisterFunction("_WXL_APPEARANCE_PREPARE", Prepare) != 0;
    ok &= f->RegisterFunction("_WXL_APPEARANCE_SELECT", Select) != 0;
    ok &= f->RegisterFunction("_WXL_APPEARANCE_CONTEXT", SetContext) != 0;
    ok &= f->RegisterFunction("_WXL_APPEARANCE_SELECTION_STATUS", SelectionStatus) != 0;
    ok &= f->RegisterFunction("_WXL_APPEARANCE_SELECTION_VISIBLE", SelectionVisibility) != 0;
    ok &= f->RegisterScript("wxl-server-appearance", Lua) != 0;
    g_api->Subscribe(uint32_t(wxl::events::Event::OnUpdate), Update, nullptr);
    g_api->Subscribe(uint32_t(wxl::events::Event::OnM2PerFrameUpdate), SelectionPerFrame, nullptr);
    g_api->Subscribe(uint32_t(wxl::events::Event::OnWorldLeave), Leave, nullptr);
    g_api->Subscribe(uint32_t(wxl::events::Event::OnWorldEnter), Enter, nullptr);
    WLOG_INFO("server-appearance: bridge installed=%u protocol=%u catalog=%u", unsigned(ok), unsigned(ap::Version), CatalogId);
    return ok;
}
} // namespace wxl_modern_m2
