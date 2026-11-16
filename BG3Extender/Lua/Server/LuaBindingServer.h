#pragma once

#include <Lua/LuaBinding.h>
#include <Lua/Osiris/Binding.h>
#include <Lua/Server/ServerEntityReplicationEvents.h>
#include <GameDefinitions/Stats/Functors.h>
#include <GameDefinitions/Status.h>
#include <Extender/Shared/ExtensionHelpers.h>
#include <Lua/Shared/LuaStats.h>
#include <Lua/Server/ServerEvents.h>

BEGIN_NS(lua)

void RegisterLibraries();
void InitObjectProxyPropertyMaps();
void InitEventRegistry();
void RegisterSharedMetatables(lua_State* L);

END_NS()

namespace bg3se::esv
{
    struct PendingHit;
    class ExtensionState;
}

BEGIN_NS(esv::lua)

using namespace ::bg3se::lua;

void RegisterServerMetatables(lua_State* L);

// What a roll is for: kept while the game evaluates a stats condition or executes a stats
// functor, read by the roll events thrown inside. Names are kept as indices; they are turned
// into strings only when an event is built.
struct RollCause
{
    EntityHandle Source;
    EntityHandle Target;
    uint32_t Spell{ FixedString::NullIndex };
    uint32_t Status{ FixedString::NullIndex };
};

// The causes open on this thread, the innermost last
std::vector<RollCause>& RollCauses();

class FunctorEventHooks
{
public:
    FunctorEventHooks(State& state);
    ~FunctorEventHooks();

private:
    State& state_;
    HitResult* OnDealDamage(bg3se::stats::DealDamageFunctor__ApplyDamageProc* next, HitResult* result, bg3se::stats::DealDamageFunctor* functor, ecs::EntityRef* casterHandle,
        ecs::EntityRef* targetHandle, glm::vec3* position, bool isFromItem, SpellIdWithPrototype* spellId,
        int storyActionId, ActionOriginator* originator, resource::GuidResourceBankBase* classResourceMgr,
        HitDesc* hit, AttackDesc* attack, EntityHandle* sourceHandle2, HitWith hitWith, int conditionRollIndex,
        bool entityDamagedEventParam, __int64 a17, SpellId* spellId2);
    void OnEntityDamageEvent(bg3se::esv::StatsSystem__ThrowDamageEventProc* next, esv::StatsSystem* statsSystem, void* temp5,
        HitDesc* hit, AttackDesc* attack, bool a5, bool a6);

    template <class TParams>
    void OnFunctorExecute(bg3se::stats::ExecuteFunctorProc<TParams>* next, HitResult* hit, bg3se::stats::Functors* self, TParams* params);
    void OnInterruptFunctorExecute(bg3se::stats::ExecuteInterruptFunctorProc* next, HitResult* hit, ecs::EntityWorld* world, bg3se::stats::Functors* self, bg3se::stats::InterruptContextData* params);
};

// The roll events (BeforeRoll, Roll, RollDie): wraps the roll manager's dice for the life of the
// server Lua state.
class RollEventHooks
{
public:
    RollEventHooks(ExtensionState& state);
    ~RollEventHooks();

private:
    ExtensionState& state_;

    uint64_t OnDrawNumber(eoc::roll::stream::DrawNumberProc* next, void* generator, uint64_t min, uint64_t max);
    int32_t OnRollDice(eoc::roll::stream::RollManager__InternalRollDiceProc* next, eoc::roll::stream::RollManager* self,
        eoc::roll::stream::Stream* stream, RollDefinition* dice, bool updateDebts, int32_t dc, bool side, float probability,
        bool maximize, StatsRollResult* result, void* perDie);
    int32_t OnRollBoosted(eoc::roll::stream::RollManager__InternalRollDiceWithBoostsProc* next, eoc::roll::stream::RollManager* self,
        void* env, void* roller, void* worldView, eoc::roll::stream::Stream* stream, Roll* roll, bool updateDebts, int32_t dc,
        bool side, float probability, int32_t critRange, bool maximize, StatsRollResult* result);
    int32_t OnRollAgainstDC(eoc::roll::stream::RollManager__RollDiceWithBoostsAgainstDCProc* next, eoc::roll::stream::RollManager* self,
        void* env, uint64_t streamKey, void* roller, void* worldView, Roll* roll, int32_t dc, bool side, uint8_t* critFlags,
        int32_t critRange, bool maximize, StatsRollResult* result);
    bool OnAdjustConditionRoll(esv::interrupt::AdjustConditionRollProc* next, void* adjustments, ConditionRoll* roll);
    uint64_t OnCheckCondition(thoth::shared::Machine__CheckStaticConditionProc* next, void* machine, void* result, int32_t index,
        void* context, uint64_t a5, uint64_t a6);
};


class ExtensionLibraryServer : public ExtensionLibrary
{
public:
    void Register(lua_State * L) override;
    void RegisterLib(lua_State * L) override;
    STDString GenerateOsiHelpers();

private:
    static char const * const NameResolverMetatableName;

    void RegisterNameResolverMetatable(lua_State * L);
    void CreateNameResolver(lua_State * L);
};

class ServerState : public State
{
public:
    static ServerState* FromLua(lua_State* L);

    ServerState(ExtensionState& state, uint32_t generationId);
    ~ServerState();

    void Initialize() override;
    bool IsClient() override;

    inline OsirisBinding& Osiris()
    {
        return osiris_;
    }

    void OnGameSessionLoaded() override;
    void StoryFunctionMappingsUpdated();

    EntityReplicationEventHooks* GetReplicationEventHooks() override;

    inline ServerEntityReplicationEventHooks& GetServerReplicationEventHooks()
    {
        return replicationHooks_;
    }

    template <class TArg>
    void Call(char const* mod, char const* func, std::vector<TArg> const & args)
    {
        VMCallEntry _(this);
        auto L = GetState();
        lua_checkstack(L, (int)args.size() + 2);
        auto stackSize = lua_gettop(L);

        try {
            if (mod != nullptr) {
                PushModFunction(L, mod, func); // stack: func
            } else {
                lua_getglobal(L, func); // stack: func
            }

            for (auto & arg : args) {
                OsiToLua(L, arg); // stack: func, arg0 ... argn
            }

            auto status = CallWithTraceback(L, (int)args.size(), 0);
            if (status != LUA_OK) {
                LuaError("Failed to call function '" << func << "': " << lua_tostring(L, -1));
                // stack: errmsg
                lua_pop(L, 1); // stack: -
            }
        } catch (Exception &) {
            auto stackRemaining = lua_gettop(L) - stackSize;
            if (stackRemaining > 0) {
                if (mod != nullptr) {
                    LuaError("Call to mod function '" << mod << "'.'" << func << "' failed: " << lua_tostring(L, -1));
                } else {
                    LuaError("Call to mod function '" << func << "' failed: " << lua_tostring(L, -1));
                }
                lua_pop(L, stackRemaining);
            } else {
                if (mod != nullptr) {
                    LuaError("Internal error during call to mod function '" << mod << "'.'" << func << "'");
                } else {
                    LuaError("Internal error during call to mod function '" << func << "'");
                }
            }
        }
    }

    std::optional<STDString> GetModPersistentVars(STDString const& modTable);
    void RestoreModPersistentVars(STDString const& modTable, STDString const& vars);

    bool Query(char const* mod, char const* name, RegistryEntry * func,
        std::vector<CustomFunctionParam> const & signature, OsiArgumentDesc & params);

private:
    ExtensionLibraryServer library_;
    OsirisBinding osiris_;
    FunctorEventHooks functorHooks_;
    RollEventHooks rollHooks_;
    ServerEntityReplicationEventHooks replicationHooks_;

    bool QueryInternal(char const* mod, char const* name, RegistryEntry * func,
        std::vector<CustomFunctionParam> const & signature, OsiArgumentDesc & params);
};

END_NS()
