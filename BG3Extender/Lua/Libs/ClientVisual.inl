#include <GameDefinitions/Components/Timeline.h>
#include <GameDefinitions/Components/Visual.h>
#include <GameDefinitions/Animation.h>
#include <GameDefinitions/Components/Effect.h>
#include <GameDefinitions/Components/Level.h>

/// <lua_module>Visual</lua_module>
BEGIN_NS(ecl::lua::visual)

// A figure that exists on the client only, built the way the game's timeline system builds the
// figures of a cinematic that have no entity of the level behind them (the adventurers of the
// main menu, the extras of a dialog): a timeline dummy made from a root template. The steps are
// the game's own, in its order:
//
//   ecl::TLPreviewDummyHelpers::CreateDummy          a new client entity with a uuid, a
//                                                    TimelineTransform and a TLPreviewDummy
//   ecl::TLPreviewDummyHelpers::SetupDummy           the components a dummy of that kind carries
//   ecl::TLPreviewDummyHelpers::PrePrepareDummy      the look of the template: visual resource,
//                                                    equipment, name
//   ecl::TLPreviewDummyHelpers::LoadDummy            asks the visual system to load the visual
//   ecl::TLPreviewDummyHelpers::AttachDummyToScene   once it is loaded, puts it into the level's
//                                                    scene (the timeline system does this in the
//                                                    callback of its load; here it is done on the
//                                                    client tick after the visual appears)
//
// No server entity is made and nothing is replicated. A level must be loaded on the client; the
// main menu has one.

// The object the game hands to LoadDummy to be told the load ended: a reference-counted holder of
// a function. `Block` points at `Functions` (the call, a copy, and a move that destroys when given
// no destination), `State` and `Event` are the game's, `RefCount` frees it at zero.
struct DummyLoadCallback
{
    void** Block;
    void* Functions[3];
    uint64_t Captures[4];
    uint32_t State;
    HANDLE Event;
    int32_t RefCount;
};

static_assert(sizeof(DummyLoadCallback) == 0x58, "the game allocates this object with 0x58 bytes");

void DummyLoadEnded()
{
    // Nothing: the tick looks for the loaded visual by itself
}

void** DummyLoadCallbackCopy(void*, void** from, void** to)
{
    to[0] = from[0];
    to[1] = from[1];
    to[2] = from[2];
    return to;
}

void** DummyLoadCallbackMove(void*, void** from, void** to)
{
    if (to == nullptr) return nullptr;
    return DummyLoadCallbackCopy(nullptr, from, to);
}

struct PendingDummy
{
    EntityHandle Entity;
    std::optional<glm::vec3> Position;
    std::optional<glm::quat> Rotation;
    bool TransformWritten{ false };
    uint32_t TicksUnseen{ 0 };
};

std::vector<PendingDummy> gPendingDummies;

// The visual scene of the level the client is in, where the game itself reads it for its dummies
void* GetClientLevelScene()
{
    auto levelManager = GetStaticSymbols().ecl__LevelManager;
    if (levelManager == nullptr || *levelManager == nullptr) return nullptr;
    auto level = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(*levelManager) + 0x90);
    if (level == nullptr) return nullptr;
    return *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(level) + 0x28);
}

// Called on every client tick: writes the place asked for and attaches each dummy whose visual
// has been loaded to the level's scene, as the timeline system does when its load ends.
void UpdateDummies(ecs::EntitySystemHelpersBase& helpers)
{
    if (gPendingDummies.empty()) return;

    auto const& sym = GetStaticSymbols();
    auto scene = GetClientLevelScene();
    ecs::EntityWorld* world = helpers.GetEntityWorld();
    if (sym.ecl__TLPreviewDummyHelpers__AttachDummyToScene == nullptr || scene == nullptr || world == nullptr) {
        gPendingDummies.clear();
        return;
    }

    for (auto it = gPendingDummies.begin(); it != gPendingDummies.end(); ) {
        auto dummy = helpers.GetComponent<TLPreviewDummy>(it->Entity);
        if (dummy == nullptr) {
            // Not there yet (made this frame), or destroyed
            if (++it->TicksUnseen > 120) {
                it = gPendingDummies.erase(it);
            } else {
                ++it;
            }
            continue;
        }

        it->TicksUnseen = 0;
        if (!it->TransformWritten) {
            auto transform = helpers.GetComponent<TransformComponent>(it->Entity);
            if (transform != nullptr) {
                if (it->Position) transform->Transform.Translate = *it->Position;
                if (it->Rotation) transform->Transform.RotationQuat = *it->Rotation;
                it->TransformWritten = true;
            }
        }

        auto visual = helpers.GetComponent<VisualComponent>(it->Entity);
        if (visual == nullptr || visual->Visual == nullptr) {
            ++it;
            continue;
        }

        if (it->Position) visual->Visual->SetWorldTranslate(*it->Position);
        if (it->Rotation) visual->Visual->SetWorldRotate(*it->Rotation);
        sym.ecl__TLPreviewDummyHelpers__AttachDummyToScene(&world, dummy, scene);
        it = gPendingDummies.erase(it);
    }
}

/// Ext.ClientVisual.CreateDummy(template, [position], [rotation], [timelineType])
///
/// Builds a figure on the client only, from a character root template, with the functions the
/// game's timeline system builds its own dummies with (the adventurers of the main menu are such
/// dummies). Answers the new entity at once; its `Visual` is filled a few frames later, when the
/// game has loaded the look, and the figure is then put into the level's scene at `position`
/// ({x, y, z}) turned by `rotation` (a quaternion {x, y, z, w}). `timelineType` is the game's
/// `eoc::timeline::ETimelineType` the dummy is made for (default 1, what a dialog's dummies carry).
///
/// The entity carries `TLPreviewDummy`, `TimelineActorData`, `Visual`, `AnimationBlueprint` and
/// the other components of a timeline dummy; no timeline owns it, so nothing moves or hides it:
/// move it with `entity.Visual.Visual:SetWorldTranslate` and `:SetWorldRotate`. The server knows
/// nothing of it and it is not saved. Remove it with `Ext.ClientVisual.DestroyDummy`.
///
/// `nil` and an error line when the template is not a character root template, when no level is
/// loaded on the client, or when the game's functions were not found.
std::optional<EntityHandle> CreateDummy(lua_State* L, FixedString templateId, std::optional<glm::vec3> position,
    std::optional<glm::quat> rotation, std::optional<uint8_t> timelineType)
{
    auto const& sym = GetStaticSymbols();
    if (sym.ecl__TLPreviewDummyHelpers__CreateDummy == nullptr || sym.ecl__TLPreviewDummyHelpers__SetupDummy == nullptr
        || sym.ecl__TLPreviewDummyHelpers__PrePrepareDummy == nullptr || sym.ecl__TLPreviewDummyHelpers__LoadDummy == nullptr
        || sym.ecl__TLPreviewDummyHelpers__AttachDummyToScene == nullptr
        || sym.ls__gGlobalResourceManager == nullptr || *sym.ls__gGlobalResourceManager == nullptr) {
        ERR("CreateDummy(): not available in this version of the game");
        return {};
    }

    auto bank = sym.GetGlobalTemplateBank();
    auto tmpl = bank != nullptr ? bank->Templates.get_or_default(templateId) : nullptr;
    if (tmpl == nullptr) {
        ERR("CreateDummy(): no root template '%s'", templateId.GetString());
        return {};
    }

    auto type = tmpl->GetType();
    if (type == nullptr || *type != FixedString("character")) {
        ERR("CreateDummy(): '%s' is not a character template", templateId.GetString());
        return {};
    }

    auto scene = GetClientLevelScene();
    auto& helpers = State::FromLua(L)->GetEntitySystemHelpers();
    ecs::EntityWorld* world = helpers.GetEntityWorld();
    if (scene == nullptr || world == nullptr) {
        ERR("CreateDummy(): no level is loaded on the client");
        return {};
    }

    auto kind = timelineType.value_or(1);
    auto uuid = Guid::Generate();
    auto dummy = sym.ecl__TLPreviewDummyHelpers__CreateDummy(&uuid, kind);
    if (dummy == nullptr) {
        ERR("CreateDummy(): the game made no dummy");
        return {};
    }

    auto entity = dummy->field_30;
    // Every component the game gives the dummy of a character template
    sym.ecl__TLPreviewDummyHelpers__SetupDummy(&entity, 0x1fc7, nullptr, kind);
    sym.ecl__TLPreviewDummyHelpers__PrePrepareDummy(&world, *sym.ls__gGlobalResourceManager, scene, dummy, tmpl);

    auto callback = static_cast<DummyLoadCallback*>(GameAllocRaw(sizeof(DummyLoadCallback)));
    memset(callback, 0, sizeof(DummyLoadCallback));
    callback->Functions[0] = &DummyLoadEnded;
    callback->Functions[1] = &DummyLoadCallbackCopy;
    callback->Functions[2] = &DummyLoadCallbackMove;
    callback->Block = callback->Functions;
    callback->RefCount = 1;

    void* reference = callback;
    sym.ecl__TLPreviewDummyHelpers__LoadDummy(&world, dummy, &reference);
    if (InterlockedDecrement(reinterpret_cast<LONG volatile*>(&callback->RefCount)) == 0) {
        if (callback->Event != nullptr) CloseHandle(callback->Event);
        GameFree(callback);
    }

    gPendingDummies.push_back(PendingDummy{ entity, position, rotation });
    return entity;
}

/// Ext.ClientVisual.DestroyDummy(entity)
///
/// Removes a dummy made with `CreateDummy`, with the function the timeline system removes its own
/// with: the visual, the physics and the light are unloaded and the entity is destroyed at the end
/// of the frame. False when the entity is not a timeline dummy.
bool DestroyDummy(lua_State* L, EntityHandle entity)
{
    auto const& sym = GetStaticSymbols();
    if (sym.ecl__TLPreviewDummyHelpers__DestroyDummy == nullptr) {
        ERR("DestroyDummy(): not available in this version of the game");
        return false;
    }

    auto& helpers = State::FromLua(L)->GetEntitySystemHelpers();
    ecs::EntityWorld* world = helpers.GetEntityWorld();
    if (world == nullptr || helpers.GetComponent<TLPreviewDummy>(entity) == nullptr) return false;

    for (auto it = gPendingDummies.begin(); it != gPendingDummies.end(); ) {
        if (it->Entity == entity) {
            it = gPendingDummies.erase(it);
        } else {
            ++it;
        }
    }

    sym.ecl__TLPreviewDummyHelpers__DestroyDummy(&world, entity.Handle);
    return true;
}

// The game's EoCGE::ASAnimation, the argument of the gameplay event of the same name: a tuple of
// (start, loop, end, float, flag) that the compiler lays out last member first. A set is a
// virtual table, an "owned" flag and the array; the game takes the arrays out of the three sets.
struct ASAnimationEvent
{
    bool Flag{ false };
    float Time{ 0.0f };
    gn::TGenomeSet<FixedString> End;
    gn::TGenomeSet<FixedString> Loop;
    gn::TGenomeSet<FixedString> Start;
};

static_assert(sizeof(gn::TGenomeSet<FixedString>) == 0x18, "a genome set is a virtual table, a flag and a pointer");
static_assert(offsetof(ASAnimationEvent, Start) == 0x38 && sizeof(ASAnimationEvent) == 0x50, "the game reads the sets at +8, +0x20 and +0x38");

AnimationBlueprintSystem* GetAnimationSystem(lua_State* L, EntityHandle entity)
{
    auto& helpers = State::FromLua(L)->GetEntitySystemHelpers();
    if (helpers.GetComponent<AnimationBlueprintComponent>(entity) == nullptr) return nullptr;
    return helpers.GetSystem<AnimationBlueprintSystem>();
}

/// Ext.ClientVisual.PlayAnimation(entity, start, [loop], [finish], [time], [flag])
///
/// Makes an entity of the client play animations, with the gameplay event the game's own "play an
/// animation" action state (`ecl::ASAnimation`, what `Osi.PlayAnimation` and
/// `Osi.PlayLoopingAnimation` end in on the client) sends to the entity's animation blueprint:
/// `ls::AnimationBlueprintSystem::TriggerGameplayEvent<EoCGE::ASAnimation>`. `start`, `loop` and
/// `finish` are lists of animation short names (a list may be empty): the blueprint plays one of
/// `start`, then keeps playing one of `loop` until the animation is stopped, then one of `finish`.
/// A short name is looked up in the entity's animation sets (`entity.AnimationWaterfall`); `time`
/// and `flag` are the event's last two arguments, passed as given (the action state passes two
/// values of its own state).
///
/// Works on anything that carries `AnimationBlueprint`: a character, a timeline dummy made with
/// `CreateDummy`. Nothing is sent to the server. False when the entity has no animation blueprint
/// or the game's function was not found.
bool PlayAnimation(lua_State* L, EntityHandle entity, Array<FixedString> start, std::optional<Array<FixedString>> loop,
    std::optional<Array<FixedString>> finish, std::optional<float> time, std::optional<bool> flag)
{
    auto trigger = GetStaticSymbols().ls__AnimationBlueprintSystem__TriggerASAnimation;
    if (trigger == nullptr) {
        ERR("PlayAnimation(): not available in this version of the game");
        return false;
    }

    auto system = GetAnimationSystem(L, entity);
    if (system == nullptr) return false;

    ASAnimationEvent event;
    event.Flag = flag.value_or(false);
    event.Time = time.value_or(0.0f);
    for (auto const& name : start) event.Start.Values->push_back(name);
    if (loop) {
        for (auto const& name : *loop) event.Loop.Values->push_back(name);
    }
    if (finish) {
        for (auto const& name : *finish) event.End.Values->push_back(name);
    }

    trigger(system, &entity, &event);
    return true;
}

/// Ext.ClientVisual.StopAnimation(entity, [flag])
///
/// Ends what `PlayAnimation` started, with the event the action state sends when it is left:
/// `ls::AnimationBlueprintSystem::TriggerGameplayEvent<EoCGE::ASCancelAnimation>`; `flag` is the
/// event's one argument, passed as given. False when the entity has no animation blueprint or the
/// game's function was not found.
bool StopAnimation(lua_State* L, EntityHandle entity, std::optional<bool> flag)
{
    auto trigger = GetStaticSymbols().ls__AnimationBlueprintSystem__TriggerASCancelAnimation;
    if (trigger == nullptr) {
        ERR("StopAnimation(): not available in this version of the game");
        return false;
    }

    auto system = GetAnimationSystem(L, entity);
    if (system == nullptr) return false;

    bool event = flag.value_or(false);
    trigger(system, &entity, &event);
    return true;
}

// What ls::EffectsManager::PlayEffect answers: the game's reference to the new effect entity (the
// entity, its world and the components it found), 14 machine words.
struct EffectEntityRef
{
    EntityHandle Entity;
    void* World{ nullptr };
    uint64_t Rest[12]{};
};

/// Ext.ClientVisual.PlayEffect(effect, position, [rotation], [scale], [levelOf])
///
/// Plays an effect (VFX) on the client with the game's own `ls::EffectsManager::PlayEffect(effect,
/// level, scene, position, rotation, scale)`: a new client entity with `Effect`, `Visual` and
/// `Transform` is made from the effect resource `effect` (its id), put at `position` ({x, y, z})
/// turned by `rotation` (a quaternion, default none) in the scene of the level the client is in,
/// and started. `levelOf` is an entity of the level (a figure made with `CreateDummy` will do): the
/// effect entity gets its `Level` when given. Answers the effect entity, or `nil` when the
/// resource does not exist, no level is loaded or the game's function was not found.
///
/// Nothing is sent to the server. The effect entity is the caller's to end: `StopEffect`.
std::optional<EntityHandle> PlayEffect(lua_State* L, FixedString effect, glm::vec3 position, std::optional<glm::quat> rotation,
    std::optional<float> scale, std::optional<EntityHandle> levelOf)
{
    auto play = GetStaticSymbols().ls__EffectsManager__PlayEffect;
    if (play == nullptr) {
        ERR("PlayEffect(): not available in this version of the game");
        return {};
    }

    auto& helpers = State::FromLua(L)->GetEntitySystemHelpers();
    auto manager = helpers.GetSystem<EffectsManager>();
    auto scene = GetClientLevelScene();
    if (manager == nullptr || scene == nullptr) {
        ERR("PlayEffect(): no level is loaded on the client");
        return {};
    }

    EntityHandle level{};
    if (levelOf) {
        auto component = helpers.GetComponent<LevelComponent>(*levelOf);
        if (component != nullptr) level = component->RootLevel;
    }

    EffectEntityRef made;
    auto turned = rotation.value_or(glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
    play(manager, &made, &effect, level.Handle, scene, &position, &turned, scale.value_or(1.0f));
    if (!made.Entity || made.World == nullptr) {
        ERR("PlayEffect(): the game made no effect from '%s'", effect.GetString());
        return {};
    }
    return made.Entity;
}

/// Ext.ClientVisual.StopEffect(entity, [how])
///
/// Ends an effect entity by queueing one of the effects manager's own orders for it, as the
/// game's `ls::EffectsManager::StopEffect`, `ForceStopEffect` and `DestroyEffect` do: `how` is
/// "Stop" (default: the effect is told to end and plays out), "ForceStop" or "Destroy". False when
/// the entity carries no `Effect`.
bool StopEffect(lua_State* L, EntityHandle entity, std::optional<FixedString> how)
{
    auto& helpers = State::FromLua(L)->GetEntitySystemHelpers();
    auto manager = helpers.GetSystem<EffectsManager>();
    auto effect = helpers.GetComponent<EffectComponent>(entity);
    if (manager == nullptr || effect == nullptr) return false;

    EffectInvoke order;
    if (how && *how == FixedString("ForceStop")) {
        order.Invoke = EffectForceStopInvoke{};
    } else if (how && *how == FixedString("Destroy")) {
        order.Invoke = EffectDestroyInvoke{};
    } else {
        order.Invoke = EffectStopInvoke{};
    }
    // The salt the game stamps an order with: bits 6..12 of the effect's construction word
    order.Salt = (effect->ConstructFlagsAndSalt >> 6) & 0x7f;
    order.Effect = entity;
    manager->Invoke(std::move(order));
    return true;
}

void RegisterVisualLib()
{
    DECLARE_MODULE(Visual, Client)
    BEGIN_MODULE()
    MODULE_FUNCTION(CreateDummy)
    MODULE_FUNCTION(DestroyDummy)
    MODULE_FUNCTION(PlayAnimation)
    MODULE_FUNCTION(StopAnimation)
    MODULE_FUNCTION(PlayEffect)
    MODULE_FUNCTION(StopEffect)
    END_MODULE()
}

END_NS()
