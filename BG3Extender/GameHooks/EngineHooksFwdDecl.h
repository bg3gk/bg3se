#pragma once

// Forward declarations to ensure we don't need to pull full headers for hook definitions

using FindFirstFileWProc = WINBASEAPI HANDLE WINAPI(_In_ LPCWSTR lpFileName, _Out_ LPWIN32_FIND_DATAW lpFindFileData);
using FindNextFileWProc = WINBASEAPI BOOL WINAPI(_In_ HANDLE hFindFile, _Out_ LPWIN32_FIND_DATAW lpFindFileData);
using FindCloseProc = WINBASEAPI BOOL WINAPI(_Inout_ HANDLE hFindFile);


BEGIN_SE()

struct HitResult;
struct HitDesc;
struct AttackDesc;

struct FileReader;
struct AiGrid;
struct Visual;

using CoreLibInitProc = void (void*);

using AiPathId = int32_t;

using FileReader__CtorProc = FileReader* (FileReader* self, Path const& path, unsigned int type, unsigned int openFlags);
using FileReader__DtorProc = void (FileReader* self);

using FileSystem__CollectFilesWithGlobProc = void (Path const& path, StringView const& glob, bool recursive, Array<Path>& paths, bool checkPackedFiles);

using AiGrid__FindPathProc = void(AiGrid* self, AiPathId pathId);
using AiGrid__FindPathImmediateProc = bool(AiGrid* self, AiPathId pathId);

using Visual__UpdateBlendshapeWeightsFromSkeletonProc = void(Visual* self);

using App__LoadGraphicSettingsProc = void (App*);
using App__CtorProc = App* (App*);
using App__UpdatePathsProc = void (App*);

using AppliedMaterial__LoadTextureProc = TextureDescriptor * (AppliedMaterial* self, FixedString const& textureGuid);
using AppliedMaterial__LoadVirtualTextureProc = void* (AppliedMaterial* self, FixedString const& textureGuid);
using AppliedMaterial__TryOverrideTexture2DParameterProc = bool (AppliedMaterial* self, FixedString const& param, FixedString const& textureGuid);

using TextureManager__UnloadTextureProc = bool (TextureManager* self, FixedString const& textureGuid);

END_SE()

BEGIN_NS(stats)

struct DealDamageFunctor;
struct Functors;

struct AttackTargetContextData;
struct AttackPositionContextData;
struct MoveContextData;
struct TargetContextData;
struct NearbyAttackedContextData;
struct NearbyAttackingContextData;
struct EquipContextData;
struct SourceContextData;
struct InterruptContextData;

using RPGStats__LoadProc = void(RPGStats* self, Array<STDString>* paths);
using RPGStats__ParseStructureFolderProc = void(RPGStats* self, Array<STDString>* paths);
using RPGStats__ParseDataBuffersProc = bool (RPGStats* self);

using Object__SetPropertyStringProc = void (Object* self, FixedString const& propertyName, char const* value);

using DealDamageFunctor__ApplyDamageProc = HitResult* (HitResult* result, DealDamageFunctor* functor, ecs::EntityRef* casterHandle,
    ecs::EntityRef* targetHandle, glm::vec3* position, bool isFromItem, SpellIdWithPrototype* spellId,
    int storyActionId, ActionOriginator* originator, resource::GuidResourceBankBase* classResourceMgr,
    HitDesc* hit, AttackDesc* attack, EntityHandle* sourceHandle2, HitWith hitWith, int conditionRollIndex,
    bool entityDamagedEventParam, __int64 a17, SpellId* spellId2);

template <class TContext>
using ExecuteFunctorProc = void (HitResult* hit, Functors* self, TContext* params);

using ExecuteInterruptFunctorProc = void(HitResult* hit, ecs::EntityWorld* world, Functors* self, InterruptContextData* params);

using SpellPrototype__InitProc = void (SpellPrototype* self, FixedString const& spellId);
using StatusPrototype__InitProc = void (StatusPrototype* self, FixedString const& statusId, uint8_t flags);
using PassivePrototype__InitProc = void (PassivePrototype* self, stats::Object* stats);
using InterruptPrototype__InitProc = void (InterruptPrototype* self, stats::Object* stats);

using ParseStaticBoostsProc = bool(LSStringView& str, Array<Guid>& boosts, void* temp);

END_NS()

BEGIN_SE()
struct Roll;
struct RollDefinition;
struct StatsRollResult;
END_SE()

BEGIN_NS(eoc::roll::stream)

struct RollManager;
struct Stream;

// The dice of the roll manager (Lua/Server/RollEvents.inl).
// One die: the generator is the Mersenne Twister of the dice set, whose +0x1390 points at the
// stream's NumRolls.
using DrawNumberProc = uint64_t (void* generator, uint64_t min, uint64_t max);
// One set of dice from a stream; result and perDie (Array<uint8_t>*) may be null.
using RollManager__InternalRollDiceProc = int32_t (RollManager* self, Stream* stream, RollDefinition* dice,
    bool updateDebts, int32_t dc, bool side, float probability, bool maximize, StatsRollResult* result, void* perDie);
// A d20 formula with the roller's boosts (dice doubled for a d20, min/max/reroll boosts, one kept).
// roller is a 0x38-byte reference: +0 EntityHandle, +8 EntityWorld*, +0x10 cached boosts, +0x30 fetch flag.
using RollManager__InternalRollDiceWithBoostsProc = int32_t (RollManager* self, void* env, void* roller, void* worldView,
    Stream* stream, Roll* roll, bool updateDebts, int32_t dc, bool side, float probability, int32_t critRange,
    bool maximize, StatsRollResult* result);
// Finds the stream by its key, then rolls (through the weighted path or RollDiceWithBoosts).
using RollManager__RollDiceWithBoostsAgainstDCProc = int32_t (RollManager* self, void* env, uint64_t streamKey,
    void* roller, void* worldView, Roll* roll, int32_t dc, bool side, uint8_t* critFlags, int32_t critRange,
    bool maximize, StatsRollResult* result);

END_NS()

BEGIN_NS(ecs)

struct EntityWorld;

using EntityWorld__UpdateProc = void (EntityWorld* self, GameTime const& time);
using EntityWorld__FlushECBsProc = bool (EntityWorld* self);

END_NS()

BEGIN_NS(esv)

struct OsirisVariableHelper;
struct SavegameVisitor;
struct StatsSystem;
struct SurfaceAction;
struct TransformSurfaceAction;

using OsirisVariableHelper__SavegameVisitProc = bool(OsirisVariableHelper*, SavegameVisitor*);

using StatsSystem__ThrowDamageEventProc = void(StatsSystem* statsSystem, void* temp5, HitDesc* hit, AttackDesc* attack, bool a5, bool a6);

using SurfaceActionFactoryCreateProc = SurfaceAction* (void* self, SurfaceActionType type, uint64_t actionHandle);
using TransformSurfaceAction__InitProc = void (TransformSurfaceAction*, SurfaceTransformActionType transformAction, SurfaceLayer8 layer, SurfaceType originSurface);

using GameStateEventManager__ExecuteGameStateChangedEventProc = void (void* self, GameState fromState, GameState toState);
using GameStateThreaded__GameStateWorker__DoWorkProc = void (void* self);
using GameStateMachine__UpdateProc = void (void* self, GameTime* time);

END_NS()

BEGIN_NS(ecl)

using EoCClient__HandleErrorProc = void (EoCClient* self, TranslatedString const& message, bool exitGame, TranslatedString const& a4, uint8_t messageBoxLocalId);

using GameStateEventManager__ExecuteGameStateChangedEventProc = void (void* self, GameState fromState, GameState toState);
using GameStateThreaded__GameStateWorker__DoWorkProc = void (void* self);
using GameStateMachine__UpdateProc = void (void* self, GameTime* time);

END_NS()

BEGIN_NS(ui)

struct DataContextProvider;
struct DeferredPredicatesManager;

using DataContextProvider__ExecuteCommandQueuesProc = void(DataContextProvider*, void* /* ls::AnyView* */);
using DeferredPredicatesManager__UpdateFromUIProc = void(DeferredPredicatesManager*);

END_NS()

BEGIN_NS(thoth::shared)

// Evaluates one compiled stats condition against a context (the caster, the target, the hit,
// the status). The condition's rolls are thrown inside it. Further arguments, if any, are passed on.
using Machine__CheckStaticConditionProc = uint64_t (void* machine, void* result, int32_t index, void* context, uint64_t a5, uint64_t a6);

END_NS()

BEGIN_SE()
struct ConditionRoll;
END_SE()

BEGIN_NS(esv::interrupt)

// Writes what the interrupts used on a roll resolved to (a set roll, an adjustment, a reroll)
// into the roll; answers whether the roll's outcome changed.
using AdjustConditionRollProc = bool (void* adjustments, ConditionRoll* roll);

END_NS()

BEGIN_NS(stats)
struct SpellPrototype;
END_NS()

BEGIN_NS(eoc::spell)

// The pieces the game itself uses to work out the chance of a spell's roll (an attack roll or a
// saving throw) for a caster and a target: see Ext.Stats.GetSpellRollChance.
using SpellPrototype__GetDefaultTextKeyProc = FixedString* (stats::SpellPrototype* self, void* unused);
using SpellPrototype__GetSpellConditionsProc = void* (stats::SpellPrototype* self, void* condition, uint8_t type, FixedString* textKey);
using HitDesc__ConstructProc = void* (void* hit);
using HitDesc__DestroyProc = void (void* hit);
using ConditionContext__ConstructProc = void* (void* self, uint64_t source, uint64_t sourceProxy, uint64_t target, uint64_t targetProxy,
    glm::vec3* sourcePosition, glm::vec3* targetPosition, void* hit, void* attack, uint32_t statusEvent, uint64_t* statusSource,
    uint32_t* status, uint8_t removeCause, uint64_t flags, uint8_t ability, uint8_t skill);
using ConditionContext__DestroyProc = void (void* self);
using CalculateModifiableSpellRollChanceProc = float (void* condition, void* context, FixedString* spell, void* modifications,
    void* stats, void* machine);

END_NS()
