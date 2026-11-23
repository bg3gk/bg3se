#pragma once

#include <GameHooks/EngineHooksFwdDecl.h>
#include <GameDefinitions/FileReader.h>

BEGIN_SE()

struct ResourceManager;
struct App;

struct IndexSymbolInfo
{
    char const* name;
    int32_t* context;
};

struct StaticSymbols : Noncopyable<StaticSymbols>
{
    std::unordered_map<int32_t*, char const*> IndexSymbolToContextMaps;
    std::unordered_map<int32_t*, IndexSymbolInfo> IndexSymbolToNameMaps;
    std::unordered_map<void const*, char const*> StaticStringRegistrantMaps;

    struct EoCLibraryInfo
    {
        uint8_t const * initFunc;
        uint8_t const * freeFunc;
        uint32_t refs;
    };

    FixedString::GetStringProc* ls__FixedString__GetString{ nullptr };
    FixedString::IncRefProc* ls__FixedString__IncRef{ nullptr };
    GlobalStringTable::MainTable::CreateFromStringProc* ls__GlobalStringTable__MainTable__CreateFromString{ nullptr };
    GlobalStringTable::MainTable::DecRefProc* ls__GlobalStringTable__MainTable__DecRef{ nullptr };
    GlobalStringTable** ls__gGlobalStringTable{ nullptr };

    ThreadRegistry::RequestThreadIndexProc* ls__ThreadRegistry__RequestThreadIndex{ nullptr };
    FileReader__CtorProc* ls__FileReader__ctor{ nullptr };
    FileReader__DtorProc* ls__FileReader__dtor{ nullptr };
    FileSystem__CollectFilesWithGlobProc* ls__FileSystem__CollectFilesWithGlob{ nullptr };
    STDString** ls__PathRoots{ nullptr };
    App__CtorProc* App__Ctor{ nullptr };
    App__UpdatePathsProc* App__UpdatePaths{ nullptr };
    // Steps of the engine's start, marked in the start's timeline (Extender/Shared/StartupTimeline.h).
    App__InitGameProc* App__InitGame{ nullptr };
    App__InitializeResourceManagerProc* App__InitializeResourceManager{ nullptr };
    PackedFileManager__LoadPackFilesProc* ls__PackedFileManager__LoadPackFiles{ nullptr };
    TranslatedStringRepository__LoadFromContextFolderProc* ls__TranslatedStringRepository__LoadFromContextFolder{ nullptr };
    // The game's read of a command-line argument's value (Extender/Client/DirectSaveLoad.h).
    ArgumentString__GetValueProc* ls__ArgumentString__GetValue{ nullptr };
    // The hint list and the game's file loader (Extender/Client/ModHints.inl), both found where
    // App::InitializeResourceManager loads Hints/Hints.lsx: ecl::HintManager::m_ptr and
    // ls::FileFormatIO::LoadFile.
    void** ecl__HintManager{ nullptr };
    FileFormatIO__LoadFileProc* ls__FileFormatIO__LoadFile{ nullptr };
    // The loading screen's draw of a hint (the start's timeline, HintShown rows).
    DCLoadingScreen__OnAdvanceHintCommandProc* gui__DCLoadingScreen__OnAdvanceHintCommand{ nullptr };
    // The loading screen's slideshow made ready (the start's timeline, WallpaperSet rows).
    LSImageSequence__OnLoadedProc* gui__LSImageSequence__OnLoaded{ nullptr };
    // The interface's per-frame update and its mods' merge block (Extender/Client/EarlyModUI.h).
    GameUI__UpdateFromUIProc* gui__GameUI__UpdateFromUI{ nullptr };
    UIModManager__RegisterModsProc* ui__ModManager__RegisterMods{ nullptr };
    TextureProvider__LoadTextureMetadataInternalProc* ui__TextureProvider__LoadTextureMetadataInternal{ nullptr };
    GUI__GetApplicationResourcesProc* Noesis__GUI__GetApplicationResources{ nullptr };
    ResourceDictionary__GetMergedDictionariesProc* Noesis__ResourceDictionary__GetMergedDictionaries{ nullptr };
    GameUI__ResetModdedLibsProc* gui__GameUI__ResetModdedLibs{ nullptr };
    UIStateMachine__ApplyModsProc* ui__StateMachine__ApplyMods{ nullptr };
    // The game's resolver of the mod list into the modules it loads (Extender/Client/GameModules.h).
    ModManager__LoadModFromSettingsProc* ls__ModManager__LoadModFromSettings{ nullptr };
    // The game's mod manager's enable and disable (Lua/Libs/ClientMod.inl, Ext.Mod.SetEnabled).
    ModManagerClient__AddModProc* ecl__ModManagerClient__AddMod{ nullptr };
    ModManagerClient__RemoveModProc* ecl__ModManagerClient__RemoveMod{ nullptr };

    ecl::EoCClient** ecl__EoCClient{ nullptr };
    esv::EoCServer** esv__EoCServer{ nullptr };

    ecl::EoCClient__HandleErrorProc* ecl__EoCClient__HandleError{ nullptr };
    ecl::CursorControl** ecl__gCursorControl{ nullptr };
    ecl::DragDropManager** ls__gDragDropManager{ nullptr };

    TranslatedStringRepository** ls__gTranslatedStringRepository{ nullptr };
    TranslatedStringKeyManager** ls__gTranslatedStringKeyManager{ nullptr };

    GameStateEventManager** ecl__gGameStateEventManager{ nullptr };
    GameStateEventManager** esv__gGameStateEventManager{ nullptr };
    ecl::GameStateThreaded__GameStateWorker__DoWorkProc* ecl__GameStateThreaded__GameStateWorker__DoWork{ nullptr };
    esv::GameStateThreaded__GameStateWorker__DoWorkProc* esv__GameStateThreaded__GameStateWorker__DoWork{ nullptr };
    ecl::GameStateMachine__UpdateProc* ecl__GameStateMachine__Update{ nullptr };
    esv::GameStateMachine__UpdateProc* esv__GameStateMachine__Update{ nullptr };
    App__LoadGraphicSettingsProc* App__LoadGraphicSettings{ nullptr };
    // The GameControl global (its movie player is polled by Extender/Shared/StartupTimeline.inl),
    // found through ecl::LoadStateMovie::Start, which loads it before calling ShowMovie.
    void** ecl__gGameControl{ nullptr };
    // The loading screen manager (Extender/Shared/StartupTimeline.inl), found through
    // ecl::GameStateStartLoading::Enter, which hands it the progress total and its load states.
    void** ls__gLoadingScreenManager{ nullptr };
    input::InputManager** ls__gInputManager{ nullptr };

    ecs::EntityWorld__UpdateProc* ecs__EntityWorld__Update{ nullptr };
    ecs::EntityWorld__FlushECBsProc* ecs__EntityWorld__FlushECBs{ nullptr };

    stats::SpellPrototypeManager** eoc__SpellPrototypeManager{ nullptr };
    stats::SpellPrototype__InitProc* eoc__SpellPrototype__Init{ nullptr };

    stats::StatusPrototypeManager** eoc__StatusPrototypeManager{ nullptr };
    stats::StatusPrototype__InitProc* eoc__StatusPrototype__Init{ nullptr };

    stats::BoostPrototypeManager** eoc__BoostPrototypeManager{ nullptr };

    stats::PassivePrototypeManager** eoc__PassivePrototypeManager{ nullptr };
    stats::PassivePrototype__InitProc* eoc__PassivePrototype__Init{ nullptr };

    stats::InterruptPrototypeManager** eoc__InterruptPrototypeManager{ nullptr };
    stats::InterruptPrototype__InitProc* eoc__InterruptPrototype__Init{ nullptr };

    DynamicStatsExpressionManager** eoc__DynamicStatsExpressionManager{ nullptr };

    stats::ParseStaticBoostsProc* eoc__ParseStaticBoosts{ nullptr };

    esv::OsirisVariableHelper__SavegameVisitProc* esv__OsirisVariableHelper__SavegameVisit{ nullptr };

    stats::DealDamageFunctor__ApplyDamageProc* stats__DealDamageFunctor__ApplyDamage{ nullptr };
    esv::StatsSystem__ThrowDamageEventProc* esv__StatsSystem__ThrowDamageEvent{ nullptr };

    // The dice of the roll manager and the condition their rolls are asked from
    eoc::roll::stream::DrawNumberProc* eoc__roll__stream__DrawNumber{ nullptr };
    eoc::roll::stream::RollManager__InternalRollDiceProc* eoc__roll__stream__RollManager__InternalRollDice{ nullptr };
    eoc::roll::stream::RollManager__InternalRollDiceWithBoostsProc* eoc__roll__stream__RollManager__InternalRollDiceWithBoosts{ nullptr };
    eoc::roll::stream::RollManager__RollDiceWithBoostsAgainstDCProc* eoc__roll__stream__RollManager__RollDiceWithBoostsAgainstDC{ nullptr };
    thoth::shared::Machine__CheckStaticConditionProc* thoth__shared__Machine__CheckStaticCondition{ nullptr };
    esv::interrupt::AdjustConditionRollProc* esv__interrupt__AdjustConditionRoll{ nullptr };

    // The chance of a spell's roll, as the game works it out (Ext.Stats.GetSpellRollChance)
    eoc::spell::SpellPrototype__GetDefaultTextKeyProc* eoc__SpellPrototype__GetDefaultTextKey{ nullptr };
    eoc::spell::SpellPrototype__GetSpellConditionsProc* eoc__SpellPrototype__GetSpellConditions{ nullptr };
    eoc::spell::HitDesc__ConstructProc* eoc__HitDesc__Construct{ nullptr };
    eoc::spell::HitDesc__DestroyProc* eoc__HitDesc__Destroy{ nullptr };
    eoc::spell::ConditionContext__ConstructProc* thoth__shared__ContextData__Construct{ nullptr };
    eoc::spell::ConditionContext__DestroyProc* thoth__shared__ContextData__Destroy{ nullptr };
    eoc::spell::CalculateModifiableSpellRollChanceProc* eoc__spell__CalculateModifiableSpellRollChance{ nullptr };

    // A client-only figure built the way the timeline system builds its dummies (Ext.ClientVisual.CreateDummy)
    ecl::TLPreviewDummyHelpers__CreateDummyProc* ecl__TLPreviewDummyHelpers__CreateDummy{ nullptr };
    ecl::TLPreviewDummyHelpers__SetupDummyProc* ecl__TLPreviewDummyHelpers__SetupDummy{ nullptr };
    ecl::TLPreviewDummyHelpers__PrePrepareDummyProc* ecl__TLPreviewDummyHelpers__PrePrepareDummy{ nullptr };
    ecl::TLPreviewDummyHelpers__LoadDummyProc* ecl__TLPreviewDummyHelpers__LoadDummy{ nullptr };
    ecl::TLPreviewDummyHelpers__AttachDummyToSceneProc* ecl__TLPreviewDummyHelpers__AttachDummyToScene{ nullptr };
    ecl::TLPreviewDummyHelpers__DestroyDummyProc* ecl__TLPreviewDummyHelpers__DestroyDummy{ nullptr };

    // The animation events of the client's ASAnimation action state (Ext.ClientVisual.PlayAnimation)
    ecl::AnimationBlueprintSystem__TriggerASAnimationProc* ls__AnimationBlueprintSystem__TriggerASAnimation{ nullptr };
    ecl::AnimationBlueprintSystem__TriggerASCancelAnimationProc* ls__AnimationBlueprintSystem__TriggerASCancelAnimation{ nullptr };
    // An effect entity made and started on the client (Ext.ClientVisual.PlayEffect)
    ecl::EffectsManager__PlayEffectProc* ls__EffectsManager__PlayEffect{ nullptr };

    stats::ExecuteFunctorProc<stats::AttackTargetContextData>* esv__ExecuteStatsFunctor_AttackTargetContext{ nullptr };
    stats::ExecuteFunctorProc<stats::AttackPositionContextData>* esv__ExecuteStatsFunctor_AttackPositionContext{ nullptr };
    stats::ExecuteFunctorProc<stats::MoveContextData>* esv__ExecuteStatsFunctor_MoveContext{ nullptr };
    stats::ExecuteFunctorProc<stats::TargetContextData>* esv__ExecuteStatsFunctor_TargetContext{ nullptr };
    stats::ExecuteFunctorProc<stats::NearbyAttackedContextData>* esv__ExecuteStatsFunctor_NearbyAttackedContext{ nullptr };
    stats::ExecuteFunctorProc<stats::NearbyAttackingContextData>* esv__ExecuteStatsFunctor_NearbyAttackingContext{ nullptr };
    stats::ExecuteFunctorProc<stats::EquipContextData>* esv__ExecuteStatsFunctor_EquipContext{ nullptr };
    stats::ExecuteFunctorProc<stats::SourceContextData>* esv__ExecuteStatsFunctor_SourceContext{ nullptr };
    stats::ExecuteInterruptFunctorProc* esv__ExecuteStatsFunctor_InterruptContext{ nullptr };

    stats::RPGStats** gRPGStats{ nullptr };
    stats::RPGStats__LoadProc* RPGStats__Load{ nullptr };
    stats::RPGStats__ParseDataBuffersProc* RPGStats__ParseDataBuffers{ nullptr };
    stats::Object__SetPropertyStringProc* stats__Object__SetPropertyString{ nullptr };

    esv::LevelManager** esv__LevelManager{ nullptr };
    ecl::LevelManager** ecl__LevelManager{ nullptr };
    GlobalTemplateManager** ls__GlobalTemplateManager{ nullptr };
    LevelCacheTemplateManager** esv__CacheTemplateManager{ nullptr };

    void** esv__gSurfaceActionFactory{ nullptr };
    esv::SurfaceActionFactoryCreateProc* esv__SurfaceActionFactory__DoCreateAction{ nullptr };
    esv::TransformSurfaceAction__InitProc* esv__TransformSurfaceAction__Init{ nullptr };

    AiGrid__FindPathProc* eoc__AiGrid__FindPath{ nullptr };
    AiGrid__FindPathImmediateProc* eoc__AiGrid__FindPathImmediate{ nullptr };

    esv::SavegameManager** esv__SavegameManager{ nullptr };

    App** AppInstance{ nullptr };

    std::map<uint8_t const *, EoCLibraryInfo> Libraries;

    ls__GlobalAllocator__GetProc* ls__GlobalAllocator__Get{ nullptr };
    ls__GlobalAllocator__AllocProc* ls__GlobalAllocator__Alloc{ nullptr };
    ls__GlobalAllocator__FreeProc* ls__GlobalAllocator__Free{ nullptr };

    resource::GuidResourceManager** eoc__gGuidResourceManager{ nullptr };
    ResourceManager** ls__gGlobalResourceManager{ nullptr };
    TextureAtlasMap** ls__gTextureAtlasMap{ nullptr };

    AppliedMaterial__LoadTextureProc* ls__AppliedMaterial__LoadTexture{ nullptr };
    TextureManager__UnloadTextureProc* ls__TextureManager__UnloadTexture{ nullptr };
    AppliedMaterial__LoadVirtualTextureProc* ls__AppliedMaterial__LoadVirtualTexture{ nullptr };
    AppliedMaterial__TryOverrideTexture2DParameterProc* ls__AppliedMaterial__TryOverrideTexture2DParameter{ nullptr };

    Visual__UpdateBlendshapeWeightsFromSkeletonProc* ls__Visual__UpdateBlendshapeWeightsFromSkeleton{ nullptr };

    std::atomic<uint32_t>* Noesis__Reflection__LockedByThreadId{ nullptr };
    SRWLOCK* Noesis__Reflection__Lock{ nullptr };
    std::atomic<uint32_t>* Noesis__Reflection__NumLocks{ nullptr };
    void* Noesis__Reflection__Data{ nullptr };

    SRWLOCK* Noesis__SymbolManager__Lock{ nullptr };
    void* Noesis__SymbolManager__Data{ nullptr };
    void** Noesis__gErrorHandler{ nullptr };
    void* Noesis__GUI__LoadXaml{ nullptr };
    void* Noesis__Visual__AddVisualChild{ nullptr };
    void* Noesis__Visual__RemoveVisualChild{ nullptr };
    void* Noesis__Visual__PointFromScreen{ nullptr };
    void* Noesis__Visual__PointToScreen{ nullptr };
    void* Noesis__VisualTreeHelper__HitTest{ nullptr };

    void* ui__DataContextProvider__ExecuteCommandQueues{ nullptr };
    void* ui__deferred__PredicatesManager__UpdateFromUI{ nullptr };

    GlobalSwitches** ls__GlobalSwitches{ nullptr };

    FindFirstFileWProc* Kernel_FindFirstFileW{ nullptr };
    FindNextFileWProc* Kernel_FindNextFileW{ nullptr };
    FindCloseProc* Kernel_FindClose{ nullptr };

    inline StaticSymbols() {}
    StaticSymbols(StaticSymbols const &) = delete;
    StaticSymbols & operator = (StaticSymbols const &) = delete;

    inline GlobalSwitches * GetGlobalSwitches() const
    {
        if (ls__GlobalSwitches && *ls__GlobalSwitches) {
            return *ls__GlobalSwitches;
        } else {
            return nullptr;
        }
    }

    inline ecl::EoCClient * GetEoCClient() const
    {
        if (ecl__EoCClient == nullptr || *ecl__EoCClient == nullptr) {
            return nullptr;
        } else {
            return *ecl__EoCClient;
        }
    }

    inline esv::EoCServer* GetEoCServer() const
    {
        if (esv__EoCServer == nullptr || *esv__EoCServer == nullptr) {
            return nullptr;
        } else {
            return *esv__EoCServer;
        }
    }

    ModManager* GetModManagerClient() const;
    ModManager* GetModManagerServer() const;

    inline stats::RPGStats* GetStats() const
    {
        if (gRPGStats && *gRPGStats) {
            return *gRPGStats;
        } else {
            return nullptr;
        }
    }

    std::optional<ecl::GameState> GetClientState() const;
    std::optional<esv::GameState> GetServerState() const;

    inline TranslatedStringRepository* GetTranslatedStringRepository() const
    {
        if (ls__gTranslatedStringRepository != nullptr) {
            return *ls__gTranslatedStringRepository;
        } else {
            return nullptr;
        }
    }

    inline TranslatedStringKeyManager* GetTranslatedStringKeyManager() const
    {
        if (ls__gTranslatedStringKeyManager != nullptr) {
            return *ls__gTranslatedStringKeyManager;
        } else {
            return nullptr;
        }
    }

    inline esv::LevelManager* GetServerLevelManager() const
    {
        if (esv__LevelManager != nullptr
            && *esv__LevelManager != nullptr) {
            return *esv__LevelManager;
        } else {
            return {};
        }
    }

    esv::Level* GetCurrentServerLevel() const;
    FixedString GetCurrentServerLevelName() const;

    inline GlobalTemplateManager* GetGlobalTemplateManager() const
    {
        if (ls__GlobalTemplateManager) {
            return *ls__GlobalTemplateManager;
        } else {
            return nullptr;
        }
    }

    inline ecl::LevelManager* GetClientLevelManager() const
    {
        if (ecl__LevelManager != nullptr
            && *ecl__LevelManager != nullptr) {
            return *ecl__LevelManager;
        } else {
            return {};
        }
    }

    ecl::Level* GetCurrentClientLevel() const;

    GlobalTemplateBank* GetGlobalTemplateBank() const;
    ResourceBank* GetCurrentResourceBank() const;

    void CanonicalizePath(STDString & path) const;
    STDString ToPath(StringView path, PathRootType root, bool canonicalize = true) const;
    FileReaderPin MakeFileReader(StringView path, PathRootType root = PathRootType::Data, bool canonicalize = true, FileMappingType mapping = FileMappingType::MemoryMapped, FileOpenFlags flags = FileOpenFlags::Default) const;
    FileReaderPin MakeFileReaderAbsolute(StringView path, FileMappingType mapping = FileMappingType::MemoryMapped, FileOpenFlags flags = FileOpenFlags::Default) const;
    void DestroyFileReader(FileReader* reader);
    bool FileExists(StringView path, PathRootType root = PathRootType::Data, bool canonicalize = true) const;
};

extern StaticSymbols* gStaticSymbols;

void InitStaticSymbols();

inline StaticSymbols& GetStaticSymbols()
{
        return *gStaticSymbols;
}

END_SE()
