// Independently owned alternate-form previews. Native calls verified against build 12340.
// Gilnean entry-point names remain stable for the existing Lua/extension ABI.
#include "../ExtensionApi.hpp"
#include "GilneanPreview.hpp"
#include "GilneanPreviewLua.hpp"
#include "GilneanChoiceMapping.hpp"
#include "GilneanScenePlacement.hpp"
#include "ModernM2.hpp"
#include "wxl/AppearanceApi.h"
#include "game/Script.hpp"
#include "game/World.hpp"
#include "game/M2.hpp"
#include "game/M2Animation.hpp"
#include "engine/events/Event.hpp"
#include "offsets/engine/Gx.hpp"
#include "offsets/game/M2.hpp"
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <random>

namespace
{
namespace owner = wxl_modern_m2;
namespace script = wxl::game::script;
namespace off = wxl::offsets::game::m2;
constexpr uint32_t MaxOptions = 64;
struct FormPair { uint32_t nativeRace, primary, alternate; };
constexpr FormPair pairs[] = {{12,22,23},{17,76,70},{30,75,52}};
const FormPair* Pair()
{
    uint32_t race=0,s=0;
    if (!owner::CustomizeIdentity(race,s)) return nullptr;
    for (const auto& pair : pairs) if (pair.nativeRace==race) return &pair;
    return nullptr;
}
uint32_t AlternateRace() { const auto* p=Pair(); return p ? p->alternate : 23; }
uint32_t boundRace=0;

std::array<std::array<std::array<uint32_t, MaxOptions>, 2>, 2> choiceBanks{};
auto& Choices(uint32_t s) { return choiceBanks[AlternateRace()==23 ? 0 : 1][s]; }
std::array<std::array<std::array<uint32_t, MaxOptions>, 2>, 2> sourceBanks{};
auto& LastSource(uint32_t s) { return sourceBanks[AlternateRace()==23 ? 0 : 1][s]; }
void* frame = nullptr;
void* component = nullptr;
void* root = nullptr;
void* sceneParent = nullptr;
void* bootstrapRoot = nullptr;
bool actorAnimated = false;
using AnimateSceneFn = void(__fastcall*)(void*, void*, void*);
AnimateSceneFn originalAnimateScene = nullptr;
using SetAnimatingFn = void(__fastcall*)(void*, void*, uint32_t);
off::M2_BuildBonePaletteFn originalBuildPalette = nullptr;
owner::gilnean::Matrix resolvedCenter{}, resolvedInverseView{};
bool resolvedPlacement = false;
void* animatingScene = nullptr;
bool actorSubmitted = false;
void Release();
uint32_t sex = 0;
bool busy = false;
unsigned bindPhase = 0;
bool BindStatus(unsigned phase, const char* message)
{
    if (bindPhase != phase)
    {
        bindPhase = phase;
        WLOG_INFO("gilnean-preview: bind stage=%u %s", phase, message);
    }
    return phase == 100;
}
bool alternateInFront = false;
void* placedRoot = nullptr;
std::array<float, 16> originalPlacement{}, appliedPlacement{};

// 4F0980 allocates and constructs a 0x530 CCharacterComponent from the native pool.
// 4F16C0 invokes its destructor, cancels composition and returns the allocation to that pool.
using CreateFn = void*(__cdecl*)();
using FreeFn = void(__cdecl*)(void*);
using PreferencesFn = void(__fastcall*)(void*, void*);
using SetPreferencesFn = bool(__fastcall*)(void*, void*, const void*, uint32_t);
using PrepFn = bool(__fastcall*)(void*, void*, uint32_t);
using DestroyFrameFn = void(__fastcall*)(void*, void*);
DestroyFrameFn originalDestroy = nullptr;
FreeFn originalFree = nullptr;
void* PrimaryRoot();
using AddHandFn = int32_t(__cdecl*)(void*,void*,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t);
using ClearHandFn = void(__cdecl*)(void*,uint32_t,uint32_t,uint32_t);
AddHandFn originalAddHand = nullptr;
ClearHandFn originalClearHand = nullptr;
struct HandChoice { uint32_t display=0, sheath=0, sheathed=0, shield=0, ranged=0, enchant=0; };
std::array<HandChoice,3> handChoices{};
std::array<HandChoice,3> appliedHands{};
void* handSource = nullptr;
uint32_t handGeneration = 1, appliedHandGeneration = 0;
uint32_t nextHandRetry = 0;
bool handsVisible = true;
void ObserveHandSource(void* source)
{
    if (handSource!=source) { handSource=source;handChoices={};++handGeneration; }
}
int32_t __cdecl AddHand(void* instance, void* item, uint32_t slot, uint32_t sheath,
                      uint32_t sheathed, uint32_t shield, uint32_t ranged, uint32_t enchant)
{
    if (instance && instance==PrimaryRoot() && wxl::game::world::CurrentMapId()<0 && item && slot>=15 && slot<=17)
    {
        ObserveHandSource(instance);
        handChoices[slot-15]={*static_cast<uint32_t*>(item),sheath,sheathed,shield,ranged,enchant};
        ++handGeneration;
    }
    return originalAddHand(instance,item,slot,sheath,sheathed,shield,ranged,enchant);
}
void __cdecl ClearHand(void* instance, uint32_t slot, uint32_t a, uint32_t b)
{
    if (instance && instance==PrimaryRoot() && wxl::game::world::CurrentMapId()<0 && slot>=15 && slot<=17)
    { ObserveHandSource(instance);handChoices[slot-15]={};++handGeneration; }
    originalClearHand(instance,slot,a,b);
}
void SyncHands()
{
    if (!root || handSource!=PrimaryRoot()) return;
    const bool visible=owner::GilneanGearVisible();
    if (appliedHandGeneration==handGeneration && handsVisible==visible) return;
    const uint32_t now=GetTickCount();
    if (handsVisible==visible && nextHandRetry && int32_t(now-nextHandRetry)<0) return;
    bool complete=true;
    for (uint32_t i=0;i<handChoices.size();++i)
    {
        originalClearHand(root,i+15,appliedHands[i].sheath,appliedHands[i].shield);
        appliedHands[i]={};
        const auto& hand=handChoices[i];
        if (!visible || !hand.display) continue;
        // Re-resolve the display through the native/retail lookup owner. Never
        // retain borrowed DBC string pointers from an earlier equip callback.
        std::array<uint32_t,25> record{};
        using InitRecord=void(__fastcall*)(void*,void*);
        using Lookup=void*(__fastcall*)(void*,void*,uint32_t,void*);
        wxl::game::Native<InitRecord>(0x008B7DA0)(record.data(),nullptr);
        if (wxl::game::Native<Lookup>(0x004CFD90)(reinterpret_cast<void*>(0x00AD3DDC),nullptr,hand.display,record.data()))
        {
            const bool added=originalAddHand(root,record.data(),i+15,hand.sheath,hand.sheathed,hand.shield,hand.ranged,hand.enchant)!=-1;
            complete &= added;
            if (added) appliedHands[i]=hand;
        }
        else complete=false;
        wxl::game::Native<InitRecord>(0x005EEB70)(record.data(),nullptr);
    }
    handsVisible=visible;
    nextHandRetry=complete ? 0 : now+500;
    if (complete) appliedHandGeneration=handGeneration;
}

void* PrimaryRoot()
{
    if (auto* primary = owner::CustomizeComponent())
        return *reinterpret_cast<void**>(static_cast<uint8_t*>(primary) + off::kOffCharComponentInstance);
    return nullptr;
}
void RestorePlacement()
{
    if (placedRoot && wxl::game::world::CurrentMapId() < 0 && PrimaryRoot() == placedRoot)
    {
        auto* placement = reinterpret_cast<float*>(static_cast<uint8_t*>(placedRoot) + off::kOffInstPlacement);
        // A stock race/camera rebuild may already have replaced our placement. Do not undo it.
        if (std::memcmp(placement, appliedPlacement.data(), sizeof(appliedPlacement)) == 0)
        {
            std::memcpy(placement, originalPlacement.data(), sizeof(originalPlacement));
            *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(placedRoot) + off::kOffInstInitFlags) |= 0x8000;
        }
    }
    placedRoot = nullptr;
    alternateInFront = false;
}
void __cdecl FreeComponent(void* cmo)
{
    if (component && cmo != component && cmo == owner::CustomizeComponent()) Release();
    if (cmo && cmo != component && cmo == owner::CustomizeComponent()) ObserveHandSource(nullptr);
    if (cmo && placedRoot == *reinterpret_cast<void**>(static_cast<uint8_t*>(cmo) + off::kOffCharComponentInstance))
    {
        placedRoot = nullptr;
        alternateInFront = false;
    }
    originalFree(cmo);
}

const WXL_AppearanceApi* Tables()
{
    return static_cast<const WXL_AppearanceApi*>(owner::g_api->GetInterface("wxl.appearance", WXL_APPEARANCE_API_VERSION));
}
bool Context(uint32_t wantedSex)
{
    uint32_t race = 0, currentSex = 2;
    return wantedSex < 2 && wxl::game::world::CurrentMapId() < 0 &&
        owner::CustomizeComponent() && owner::CustomizeIdentity(race, currentSex) &&
        Pair() && currentSex == wantedSex;
}
void Dirty()
{
    if (component) *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(component) + off::kOffCharComponentRebuild) |=
        off::kCharRebuildSheet | off::kCharRebuildGeosets;
}
using owner::gilnean::MatchLinkedChoice;
using owner::gilnean::SameAxis;
void Mirror(uint32_t s, bool fromGilnean, uint32_t changed = MaxOptions)
{
    const auto* t = Tables(); if (!t || !Context(s)) return;
    const uint32_t wolf = t->ChrModelForRace(Pair()->primary,s);
    const uint32_t sourceCount = fromGilnean ? owner::GilneanOptionCount(s) : t->OptionCount(wolf);
    const uint32_t targetCount = fromGilnean ? t->OptionCount(wolf) : owner::GilneanOptionCount(s);
    uint32_t settled[MaxOptions]{};
    const uint32_t settledCount = fromGilnean ? 0 :
        owner::SettledChoicesFor(owner::CustomizeComponent(), wolf, settled, MaxOptions);
    bool dirty = false;
    for (uint32_t i = 0; i < sourceCount && i < MaxOptions; ++i)
    {
        if (fromGilnean && changed != i) continue;
        WXL_ChrOption source{};
        if (!(fromGilnean ? owner::GilneanOptionAt(s,i,&source) : t->OptionAt(wolf,i,&source))) continue;
        int selected = fromGilnean ? int(Choices(s)[i]) : owner::CustomizeChoiceFor(wolf,i);
        WXL_ChrChoice value{};
        if (selected < 0)
        {
            // Automatic means the native creation selection, not choice zero.
            // Settled entries are choice IDs; locate by option membership rather
            // than assuming the recipe's order is the UI's option order.
            for (uint32_t k = 0; k < t->ChoiceCount(source.id) && selected < 0; ++k)
            {
                WXL_ChrChoice c{}; if (!t->ChoiceAt(source.id, k, &c)) continue;
                for (uint32_t n = 0; n < settledCount; ++n)
                    if (settled[n] == c.id) { selected = int(k); break; }
            }
            if (selected < 0) continue; // Wait for the primary model's first recipe.
        }
        if (!t->ChoiceAt(source.id, uint32_t(selected), &value)) continue;
        if (!fromGilnean && LastSource(s)[i] == value.id) continue;
        if (!fromGilnean) LastSource(s)[i] = value.id;
        for (uint32_t j = 0; j < targetCount && j < MaxOptions; ++j)
        {
            WXL_ChrOption target{};
            if (!(fromGilnean ? t->OptionAt(wolf,j,&target) : owner::GilneanOptionAt(s,j,&target))) continue;
            const char* a=t->OptionName(source.id), *b=t->OptionName(target.id);
            const bool dracthyr=AlternateRace()!=23;
            if (dracthyr)
            {
                // Only shared semantic axes. Numbered horns/faces are NOT equivalent across meshes.
                if (!a || !b) continue;
                const bool color=(!std::strcmp(a,"Scale Color") && !std::strcmp(b,"Primary Color")) ||
                    (!std::strcmp(b,"Scale Color") && !std::strcmp(a,"Primary Color"));
                const bool shared=!std::strcmp(a,b) && (!std::strcmp(a,"Eye Color") ||
                    !std::strcmp(a,"Eyesight") || !std::strcmp(a,"Horn Color") || !std::strcmp(a,"Jewelry Color"));
                if (!color && !shared) continue;
            }
            else if (!SameAxis(a,b)) continue;
            const int matched = dracthyr ? owner::gilnean::MatchChoice(t,value,target.id) : MatchLinkedChoice(t,value,source.id,target.id);
            if (matched < 0) continue;
            if (fromGilnean)
            {
                if (owner::SetLinkedWorgenChoice(wolf,j,uint32_t(matched)))
                { WXL_ChrChoice c{}; if (t->ChoiceAt(target.id,matched,&c)) LastSource(s)[j] = c.id; }
            }
            else if (Choices(s)[j] != uint32_t(matched))
            {
                Choices(s)[j] = uint32_t(matched); dirty = true;
                WXL_ChrChoice linked{};
                if (t->ChoiceAt(target.id, matched, &linked))
                    WLOG_INFO("gilnean-preview: linked sex=%u source=%u:%u target=%u:%u",
                              s, source.id, value.id, target.id, linked.id);
            }
        }
    }
    if (dirty) Dirty(); // One composition request for a whole Randomize operation.
}
void Release()
{
    RestorePlacement();
    if (component) owner::ReleaseGilneanEquipment(component);
    if (component) owner::ForgetCustomizationComponent(component, root);
    void* oldComponent = component;
    void* oldRoot = root;
    if (oldRoot) wxl::game::Native<SetAnimatingFn>(off::kSetAnimating)(oldRoot, nullptr, 0);
    component = frame = root = sceneParent = bootstrapRoot = nullptr;
    actorAnimated = false;
    appliedHandGeneration = 0;
    appliedHands = {};
    nextHandRetry = 0;
    resolvedPlacement = false;
    animatingScene = nullptr;
    if (oldComponent) wxl::game::Native<FreeFn>(0x004F16C0)(oldComponent);
    if (oldRoot) wxl::game::m2::ReleaseRenderCtx(oldRoot); // our GetRenderCtx reference
}
void PlaceActor()
{
    if (!root || !component || !resolvedPlacement) return;
    const auto matrix = alternateInFront ? resolvedCenter : owner::gilnean::Side(resolvedCenter, resolvedInverseView);
    std::memcpy(static_cast<uint8_t*>(root) + off::kOffInstPlacement, matrix.data(), sizeof(matrix));
    *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(root) + off::kOffInstInitFlags) |= 0x8000;
}
void __fastcall BuildPalette(void* self, void* edx, void* view, void* scale, void* translation,
                            uint32_t a, uint32_t b)
{
    if (self == sceneParent && root && animatingScene && view)
    {
        using namespace owner::gilnean;
        auto* placement = reinterpret_cast<float*>(static_cast<uint8_t*>(self) + off::kOffInstPlacement);
        Matrix local{}, attachmentView{}, sceneView{}, inverseAttachment{};
        std::memcpy(local.data(), placement, sizeof(local));
        if (self == placedRoot && local == appliedPlacement) local = originalPlacement;
        std::memcpy(attachmentView.data(), view, sizeof(attachmentView));
        std::memcpy(sceneView.data(), static_cast<uint8_t*>(animatingScene) + 0x84, sizeof(sceneView));
        if (Inverse(sceneView, resolvedInverseView) && Inverse(attachmentView, inverseAttachment))
        {
            // The primary is a child of the creation scene, NOT a scene root.
            // Its native palette input contains the resolved scene attachment.
            resolvedCenter = Multiply(Multiply(local, attachmentView), resolvedInverseView);
            resolvedPlacement = true;
            if (alternateInFront)
            {
                originalPlacement = local;
                appliedPlacement = Multiply(Multiply(Side(resolvedCenter, resolvedInverseView), sceneView), inverseAttachment);
                placedRoot = self;
                std::memcpy(placement, appliedPlacement.data(), sizeof(appliedPlacement));
                *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(self) + off::kOffInstInitFlags) |= 0x8000u;
            }
        }
    }
    originalBuildPalette(self, edx, view, scale, translation, a, b);
}
void __cdecl SceneUpdate(void*, const void* raw)
{
    if (!raw || !root || !Context(sex) || boundRace != AlternateRace()) return;
    const auto& args = *static_cast<const wxl::events::M2PerFrameUpdateArgs*>(raw);
    // Submit AFTER primary animation resolves its scene attachment. The scene's
    // worker join precedes this per-frame callback, so no concurrent actor build
    // or previous-frame matrix is involved. New head insertion does not change
    // the current node's next link; process this actor here, then native lighting
    // and draw-list construction consume both queued roots normally.
    if (args.renderCtx == sceneParent && animatingScene && resolvedPlacement && !actorSubmitted)
    {
        actorSubmitted = true;
        PlaceActor();
        if (wxl::game::Native<off::M2_IsDrawableFn>(off::kIsDrawable)(root, nullptr, 1, 1))
        {
            wxl::game::Native<SetAnimatingFn>(off::kSetAnimating)(root, nullptr, 1);
            *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(root) + off::kOffInstInitFlags) |= 0x10008u;
            float scale[3]{1,1,1}, translation[3]{};
            wxl::game::Native<off::M2_AnimateMTFn>(off::kBuildBonePalette)(root,
                static_cast<uint8_t*>(animatingScene) + 0x84, scale, translation, 1.0f, 1.0f);
            wxl::game::Native<off::M2_PerFrameUpdateFn>(off::kM2PerFrameUpdate)(root, nullptr);
        }
    }
    if (args.renderCtx == root || args.renderCtx == sceneParent)
    {
        for (uint32_t type = 2; type < 32; ++type)
            if (void* texture = owner::CustomizationWholeTexture(root, type))
                wxl::game::m2::BindTexSlotType(root, type, texture);
    }
}
void __fastcall AnimateScene(void* scene, void* edx, void* cameraContext)
{
    // Keep the independent actor out of the first animation pass. Its current
    // placement depends on Worgen's scene attachment, resolved during that pass.
    // SceneUpdate submits and processes it before lighting/draw-list construction.
    if (root && component && Context(sex) && boundRace == AlternateRace() && PrimaryRoot() == sceneParent &&
        *reinterpret_cast<void**>(static_cast<uint8_t*>(root) + off::kOffInstScene) == scene &&
        !*reinterpret_cast<void**>(static_cast<uint8_t*>(root) + off::kOffInstParent))
    {
        animatingScene = scene;
        actorSubmitted = false;
        resolvedPlacement = false;
        wxl::game::Native<SetAnimatingFn>(off::kSetAnimating)(root, nullptr, 0);
    }
    originalAnimateScene(scene, edx, cameraContext);
    if (animatingScene == scene) animatingScene = nullptr;
}

void __fastcall DestroyFrame(void* self, void* edx)
{
    if (self == frame) Release();
    originalDestroy(self, edx);
}

// GetObjectThis's real ABI has the Lua state in ESI (not a cdecl argument).
// The stock Model.SetModel wrapper at 960530 demonstrates both this and the type-id slot.
__declspec(noinline) void* FrameSelf(void* state)
{
    int* type = reinterpret_cast<int*>(wxl::offsets::engine::gx::kSimpleModelTypeId);
    if (!*type) return nullptr; // SetModel has already established this type before our method.
    int id = *type;
    void* result = nullptr;
    uintptr_t entry = 0x004A81B0;
    __asm {
        push esi
        mov esi, state
        push id
        call entry
        add esp, 4
        mov result, eax
        pop esi
    }
    return result;
}
bool Bind(void* self, uint32_t wantedSex)
{
    if (!self || !Context(wantedSex)) return BindStatus(1, "waiting for primary Worgen identity");
    void* modelRoot = *reinterpret_cast<void**>(static_cast<uint8_t*>(self) + 0x2A0);
    if (!modelRoot) return BindStatus(2, "waiting for frame model");
    void* shared = *reinterpret_cast<void**>(static_cast<uint8_t*>(modelRoot) + off::kOffInstShared);
    if (!shared || !wxl::modern::assets::m2::IsNativeLoaded(shared)) return BindStatus(3, "waiting for model data");
    const auto* skin = wxl::game::m2::M2Model(shared).GetSkin();
    if (!skin || !skin->submeshes || !skin->submeshCount) return BindStatus(4, "waiting for skin data");
    if (modelRoot == *reinterpret_cast<void**>(static_cast<uint8_t*>(owner::CustomizeComponent()) + off::kOffCharComponentInstance)) return false;
    const auto* t = Tables();
    WXL_Recipe recipe{};
    if (!t || !t->BuildForCharacter(AlternateRace(), wantedSex, nullptr, 0, &recipe)) return false;
    const char* expected = owner::ResolveModel(recipe.modelFileDataId);
    const char* actual = wxl::game::m2::M2Model(shared).GetPathStem();
    if (!expected || !actual) return false;
    // Both are resolved HD paths here; tolerate only slash and extension spelling.
    auto sameStem = [](const char* a, const char* b) {
        for (; *a && *b && *a != '.' && *b != '.'; ++a, ++b)
        {
            unsigned char x = static_cast<unsigned char>(*a), y = static_cast<unsigned char>(*b);
            if (x == '/') x = '\\'; if (y == '/') y = '\\';
            if (x >= 'A' && x <= 'Z') x += 'a' - 'A';
            if (y >= 'A' && y <= 'Z') y += 'a' - 'A';
            if (x != y) return false;
        }
        return (!*a || *a == '.') && (!*b || *b == '.');
    };
    if (!sameStem(actual, expected))
    {
        if (bindPhase != 5) WLOG_WARN("gilnean-preview: path mismatch actual=%s expected=%s", actual, expected);
        return BindStatus(5, "frame model does not match Gilnean recipe");
    }
    if (frame == self && bootstrapRoot == modelRoot && sceneParent == PrimaryRoot() && sex == wantedSex && boundRace == AlternateRace() && component) return true;
    Release();
    sceneParent = PrimaryRoot();
    void* scene = *reinterpret_cast<void**>(static_cast<uint8_t*>(sceneParent) + off::kOffSceneNodeOwner);
    if (!scene) { sceneParent = nullptr; return false; }
    root = wxl::game::m2::GetRenderCtx(scene, const_cast<char*>(expected));
    if (!root) { sceneParent = nullptr; return false; }
    // GetRenderCtx already inserts this actor in the scene list. Keep it a root:
    // slot attachment would apply Worgen facing/scale/translation a second time.
    if (*reinterpret_cast<void**>(static_cast<uint8_t*>(root) + off::kOffInstParent) != nullptr)
    { Release(); return false; }
    component = wxl::game::Native<CreateFn>(0x004F0980)();
    if (!component) { Release(); return false; }
    frame = self; bootstrapRoot = modelRoot; sex = wantedSex; boundRace = AlternateRace();
    std::array<uint32_t, 0x178 / 4> prefs{};
    wxl::game::Native<PreferencesFn>(0x004DFDA0)(prefs.data(), nullptr);
    prefs[0] = 1; // Native allocation plumbing only; owned recipe/equipment identity is boundRace.
    prefs[1] = wantedSex;
    prefs[2] = owner::CustomizeClass();
    prefs[0x20 / 4] = reinterpret_cast<uintptr_t>(root);
    // SetPreferences transfers one root reference into the component. Model.Release at 824ED0
    // decrements the first word; the frame retains its independent existing reference.
    ++*static_cast<uint32_t*>(root);
    if (!wxl::game::Native<SetPreferencesFn>(0x004F24D0)(component, nullptr, prefs.data(), 1))
    {
        Release();
        return false;
    }
    Dirty();
    WLOG_INFO("gilnean-preview: bound frame=%p component=%p root=%p sex=%u ChrModel=%u",
        frame, component, root, sex, t->ChrModelForRace(AlternateRace(), sex));
    return BindStatus(100, "independent compositor ready");
}
int __cdecl Update(void* state)
{
    bool ok = false;
    if (state && !busy && script::ArgCount(state) >= 2 && script::IsNumber(state, 2))
    {
        const double raw = script::ToNumber(state, 2);
        if (raw == 0 || raw == 1)
        {
            void* self = FrameSelf(state);
            busy = true;
            ok = Bind(self, static_cast<uint32_t>(raw));
            if (ok && !alternateInFront) Mirror(static_cast<uint32_t>(raw), false);
            if (ok) { owner::SyncGilneanEquipment(owner::CustomizeComponent(),component); SyncHands(); }
            if (ok) ok = wxl::game::Native<PrepFn>(off::kCharRenderPrep)(component, nullptr, 1);
            if (ok && !actorAnimated && wxl::game::m2animation::ModelHasSequence(root, 0))
            {
                wxl::game::Native<off::M2_SetBoneSequenceFn>(off::kSetBoneSequence)(
                    root, nullptr, 0xFFFFFFFFu, 0, 0xFFFFFFFFu, 0, 1.0f, 1, 1);
                actorAnimated = true;
            }
            if (ok) PlaceActor();
            busy = false;
        }
    }
    script::PushBoolean(state, ok);
    script::PushBoolean(state, ok);
    return 2;
}
int __cdecl Clear(void* state)
{
    if (FrameSelf(state) == frame && !busy) Release();
    return 0;
}
int __cdecl SetFront(void* state)
{
    bool ok = false;
    if (script::ArgCount(state) == 1 && script::IsNumber(state, 1))
    {
        const double value = script::ToNumber(state, 1);
        if (value == 0) { RestorePlacement(); ok = true; }
        else if (value == 1 && component && Context(sex))
        {
            if (void* primary = PrimaryRoot())
            {
                alternateInFront = true;
                ok = true;
            }
        }
    }
    script::PushBoolean(state, ok);
    return 1;
}
int __cdecl PreviewContext(void* state)
{
    uint32_t race = 0, bodySex = 0;
    if (wxl::game::world::CurrentMapId() >= 0 || !owner::CustomizeComponent() ||
        !owner::CustomizeIdentity(race, bodySex)) return 0;
    script::PushNumber(state, race);
    script::PushNumber(state, bodySex);
    if (!Pair() || bodySex > 1) return 2;
    const auto* t = Tables();
    WXL_Recipe recipe{};
    const char* path = nullptr;
    if (t && t->BuildForCharacter(AlternateRace(), bodySex, nullptr, 0, &recipe))
        path = owner::ResolveModel(recipe.modelFileDataId);
    script::PushString(state, path ? path : "");
    script::PushNumber(state, recipe.modelFileDataId);
    return 4;
}
int __cdecl Trace(void* state)
{
    if (script::ArgCount(state) == 1 && script::IsString(state, 1))
    {
        static unsigned count = 0;
        if (count++ < 512) WLOG_INFO("gilnean-ui: %.320s", script::ToString(state, 1));
    }
    return 0;
}
int __cdecl Randomize(void* state)
{
    bool ok = false;
    if (alternateInFront && Context(sex) && !busy)
    {
        const auto* t = Tables();
        if (t)
        {
            static std::mt19937 random(GetTickCount());
            const uint32_t count = owner::GilneanOptionCount(sex);
            for (uint32_t i = 0; i < count && i < MaxOptions; ++i)
            {
                WXL_ChrOption option{};
                if (!owner::GilneanOptionAt(sex, i, &option)) continue;
                const char* name=t->OptionName(option.id); if (!name || !*name) continue;
                uint32_t eligible = 0;
                for (uint32_t j = 0; j < t->ChoiceCount(option.id); ++j)
                {
                    WXL_ChrChoice value{};
                    if (!t->ChoiceAt(option.id, j, &value) || !owner::gilnean::Selectable(t, value)) continue;
                    if (std::uniform_int_distribution<uint32_t>(1, ++eligible)(random) == 1) Choices(sex)[i] = j;
                }
            }
            for (uint32_t i = 0; i < count && i < MaxOptions; ++i) Mirror(sex, true, i);
            Dirty();
            ok = true;
        }
    }
    script::PushBoolean(state, ok);
    return 1;
}
int __cdecl Option(void* state)
{
    if (script::ArgCount(state) < 2 || !script::IsNumber(state, 1) || !script::IsNumber(state, 2)) return 0;
    const double rawSex = script::ToNumber(state, 1), rawIndex = script::ToNumber(state, 2);
    if ((rawSex != 0 && rawSex != 1) || !std::isfinite(rawIndex) || rawIndex < 0 || rawIndex >= MaxOptions || std::floor(rawIndex) != rawIndex) return 0;
    const auto s = static_cast<uint32_t>(rawSex), index = static_cast<uint32_t>(rawIndex);
    if (!Context(s)) return 0;
    const auto* t = Tables(); WXL_ChrOption option{};
    if (!t || !owner::GilneanOptionAt(s, index, &option)) return 0;
    uint32_t count = t->ChoiceCount(option.id);
    if (!count) return 0;
    uint32_t& selected = Choices(s)[index];
    if (selected >= count) selected = 0;
    selected = owner::gilnean::RequiredHornIndex(t, option.id, selected);
    if (alternateInFront && script::ArgCount(state) == 3 && script::IsNumber(state, 3))
    {
        double delta = script::ToNumber(state, 3);
        if (delta == 1 || delta == -1)
        {
            const int next = owner::gilnean::NextSelectable(t, option.id, int(selected), int(delta));
            if (next < 0) return 0;
            selected = uint32_t(next);
            if (s == sex) Dirty();
            Mirror(s, true, index);
        }
    }
    WXL_ChrChoice choice{};
    if (!t->ChoiceAt(option.id, selected, &choice)) return 0;
    script::PushString(state, t->OptionName(option.id));
    script::PushString(state, t->ChoiceName(choice.id));
    script::PushNumber(state, selected + 1);
    script::PushNumber(state, count);
    script::PushNumber(state, owner::gilnean::EffectiveSwatch(choice));
    return 5;
}
}
namespace wxl_modern_m2
{
bool GilneanInFront() { return alternateInFront && Context(sex); }
bool AlternateOptionAt(uint32_t alternateRace, uint32_t s, uint32_t index, WXL_ChrOption* out)
{
    const auto* t = Tables(); if (!t || s > 1 || !out) return false;
    const uint32_t model = t->ChrModelForRace(alternateRace,s), base = t->OptionCount(model);
    if (index < base) return t->OptionAt(model,index,out) != 0;
    // Same mesh, different body layouts. Only Eye Color is borrowed. Whole eye
    // atlas layers are resolved against the Human layout in BindWholeTextures;
    // Human body layers must never be painted onto the Gilnean skin sheet.
    if (alternateRace!=23 || index >= base + 1) return false;
    static std::array<std::array<WXL_ChrOption, 3>, 2> eyes{};
    if (eyes[s][index-base].id) { *out = eyes[s][index-base]; return true; }
    WXL_Recipe r{};
    if (!t->BuildForCharacter(alternateRace,s,nullptr,0,&r)) return false;
    const uint32_t file = r.modelFileDataId;
    if (!t->BuildForCharacter(1,s,nullptr,0,&r) || r.modelFileDataId != file) return false;
    const char* names[] = { "Eye Color", "Eyesight", "Eye Style" };
    const uint32_t human = t->ChrModelForRace(1,s);
    for (uint32_t i = 0; i < t->OptionCount(human); ++i)
    {
        WXL_ChrOption o{}; if (!t->OptionAt(human,i,&o)) continue;
        const char* name = t->OptionName(o.id);
        if (name && !std::strcmp(name,names[index-base])) { eyes[s][index-base]=o; *out=o; return true; }
    }
    return false;
}
bool GilneanOptionAt(uint32_t s, uint32_t index, WXL_ChrOption* out)
{ return AlternateOptionAt(AlternateRace(), s, index, out); }
uint32_t AlternateOptionCount(uint32_t retailRace, uint32_t s)
{
    const auto* t = Tables(); if (!t || s > 1) return 0;
    const uint32_t base = t->OptionCount(t->ChrModelForRace(retailRace,s));
    WXL_ChrOption o{};
    return AlternateOptionAt(retailRace,s,base,&o) ? base+1 : base;
}
bool AlternateFormIdentity(const void* cmo, uint32_t& retailRace, uint32_t& s)
{
    return GilneanPreviewIdentity(cmo, retailRace, s) || ServerAlternateAppearanceIdentity(cmo, retailRace, s);
}
bool GilneanCustomizationPending()
{
    if (!component || !Pair()) return false;
    return busy || ((*reinterpret_cast<const uint8_t*>(static_cast<const uint8_t*>(component) +
        off::kOffCharComponentRebuild) & (off::kCharRebuildSheet | off::kCharRebuildGeosets)) != 0);
}
uint32_t GilneanOptionCount(uint32_t s)
{
    const auto* t = Tables(); if (!t || s > 1) return 0;
    const uint32_t base = t->OptionCount(t->ChrModelForRace(AlternateRace(),s));
    WXL_ChrOption o{};
    return GilneanOptionAt(s,base,&o) ? base+1 : base;
}
bool GilneanPreviewIdentity(const void* cmo, uint32_t& retail, uint32_t& bodySex)
{
    if (!component || cmo != component || wxl::game::world::CurrentMapId() >= 0) return false;
    retail = boundRace; bodySex = sex; return true;
}
int GilneanPreviewChoice(uint32_t model, uint32_t index)
{
    const auto* t = Tables();
    return component && wxl::game::world::CurrentMapId() < 0 && index < MaxOptions && t &&
        model == t->ChrModelForRace(AlternateRace(), sex) ? int(Choices(sex)[index]) : -1;
}
uint32_t GilneanChoices(uint32_t s, uint32_t* out, uint32_t capacity)
{
    const auto* t = Tables();
    if (s > 1 || !out || !t) return 0;
    uint32_t model = t->ChrModelForRace(AlternateRace(), s), count = GilneanOptionCount(s);
    if (!model || !count || count > MaxOptions || count > capacity) return 0;
    for (uint32_t i = 0; i < count; ++i)
    {
        WXL_ChrOption o{}; WXL_ChrChoice c{};
        if (!GilneanOptionAt(s, i, &o)) return 0;
        Choices(s)[i] = gilnean::RequiredHornIndex(t, o.id, Choices(s)[i]);
        if (!t->ChoiceAt(o.id, Choices(s)[i], &c)) return 0;
        out[i] = c.id;
    }
    return count;
}
uint32_t GilneanPreviewChoices(const void* cmo, uint32_t model, uint32_t* out, uint32_t capacity)
{
    const auto* t = Tables();
    if (!component || cmo != component || !t || wxl::game::world::CurrentMapId() >= 0 || model != t->ChrModelForRace(AlternateRace(), sex)) return 0;
    return GilneanChoices(sex, out, capacity);
}
void ResetGilneanChoices() { choiceBanks = {}; sourceBanks = {}; Dirty(); }
bool InstallGilneanPreview()
{
    if (!ConfigBool("WXL_M2_WORGEN_DUAL_PREVIEW", false)) return true;
    const auto* f = FrameScript();
    if (!f) return false;
    // Fail before exposing any Lua surface if the unhooked native ABI is not the verified one.
    struct Signature { uintptr_t address; const char* bytes; size_t size; };
    const Signature signatures[] = {
        {0x0095F540, "\x55\x8b\xec\x56\x8b\xf1", 6},
        {0x004BFCE0, "\x55\x8b\xec\x53\x8b\x5d\x08", 7},
        {0x0095F4F0, "\x55\x8b\xec\x56\x57\x8b\x7d\x08", 8},
        {0x004C1290, "\x55\x8b\xec\x8b\x4d\x0c", 6},
        {0x004C12B0, "\x55\x8b\xec\x83\xec\x18", 6},
        {0x004F24D0, "\x55\x8b\xec\x53\x8b\xd9", 6},
        {off::kSetAnimating, "\x55\x8b\xec\x56\x8b\xf1", 6},
        {0x004DFDA0, "\x8b\xc1\x83\x60\x24\xfc", 6},
        {0x004A81B0, "\x55\x8b\xec\x6a\x01\x56", 6},
        {0x0095F3A0, "\x56\x8b\xf1\x8b\x86\xa4\x02\x00\x00", 9},
        {off::kSetWorldTransformSimple, "\x55\x8b\xec\xd9\xe8\x56\x57", 7},
    };
    for (const auto& signature : signatures)
        if (std::memcmp(reinterpret_cast<const void*>(signature.address), signature.bytes, signature.size))
        {
            WLOG_WARN("gilnean-preview: native signature mismatch at %#x; preview disabled", unsigned(signature.address));
            return false;
        }
    // SimpleModel base destructor, before it destroys its scene or releases the frame's model.
    if (!HookAttach("GilneanPreview.FrameDestroy", 0x0095F3A0, &DestroyFrame, &originalDestroy)) return false;
    if (!HookAttach("GilneanPreview.Free", off::kCharFreeComponent, &FreeComponent, &originalFree)) return false;
    if (!HookAttach("GilneanPreview.Submit", off::kSceneAnimate, &AnimateScene, &originalAnimateScene)) return false;
    if (!HookAttach("GilneanPreview.ResolvedPlacement", off::kBuildBonePalette, &BuildPalette, &originalBuildPalette)) return false;
    if (!HookAttach("GilneanPreview.HandItem", off::kCharAddHandItem, &AddHand, &originalAddHand)) return false;
    if (!HookAttach("GilneanPreview.ClearHand", 0x004EB070, &ClearHand, &originalClearHand)) return false;
    if (!owner::g_api->Subscribe) return false;
    owner::g_api->Subscribe(uint32_t(wxl::events::Event::OnM2PerFrameUpdate), SceneUpdate, nullptr);
    return f->RegisterFunction("_WXL_GILNEAN_UPDATE", Update) &&
        f->RegisterFunction("_WXL_GILNEAN_CLEAR", Clear) &&
        f->RegisterFunction("_WXL_GILNEAN_OPTION", Option) &&
        f->RegisterFunction("_WXL_GILNEAN_RANDOMIZE", Randomize) &&
        f->RegisterFunction("_WXL_GILNEAN_CONTEXT", PreviewContext) &&
        f->RegisterFunction("_WXL_GILNEAN_FRONT", SetFront) &&
        f->RegisterFunction("_WXL_GILNEAN_TRACE", Trace) &&
        f->RegisterScript("wxl-gilnean-preview", kGilneanPreviewLua);
}
}
