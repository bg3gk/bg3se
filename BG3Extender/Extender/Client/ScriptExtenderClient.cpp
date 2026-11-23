#include <stdafx.h>
#include <Extender/Client/ExtensionStateClient.h>
#include <Extender/Client/ScriptExtenderClient.h>
#include <Lua/Debugger/LuaDebugger.h>
#include <GameDefinitions/Stats/Cache.h>
#include <Extender/ScriptExtender.h>
#include <Extender/Shared/ExtensionState.h>
#include <Extender/Version.h>
#include <shlwapi.h>

#define STATIC_HOOK(name) decltype(bg3se::ecl::ScriptExtender::name) * decltype(bg3se::ecl::ScriptExtender::name)::gHook;
STATIC_HOOK(gameStateWorkerStart_)
STATIC_HOOK(gameStateMachineUpdate_)

#include <Extender/Shared/ThreadedExtenderState.inl>
#include <Extender/Client/GameModules.inl>
#include <Extender/Client/StartupVideos.inl>
#include <Extender/Client/ModHints.inl>
#include <Extender/Client/EarlyModUI.inl>
#include <Extender/Client/DirectSaveLoad.inl>
#include <Extender/Shared/StartupTimeline.inl>
#include <Extender/Shared/LoadTrace.h>
#include <Extender/Shared/FastLoad.inl>
#include <Extender/Shared/RenderCut.h>

namespace bg3se::lua::debug
{
    // Lua/Libs/DebugCover.inl
    void CoverAtStartState(char const* state);
}

BEGIN_SE()

void InitCrashReporting();

extern char const* BuildDate;

END_SE()

BEGIN_NS(ecl)

char const * GameStateNames[] =
{
    "Unknown",
    "Init",
    "InitMenu",
    "InitNetwork",
    "InitConnection",
    "Idle",
    "LoadMenu",
    "Menu",
    "Exit",
    "SwapLevel",
    "LoadLevel",
    "LoadModule",
    "LoadSession",
    "UnloadLevel",
    "UnloadModule",
    "UnloadSession",
    "Paused",
    "PrepareRunning",
    "Running",
    "Disconnect",
    "Join",
    "Save",
    "StartLoading",
    "StopLoading",
    "StartServer",
    "Movie",
    "Installation",
    "ModReceiving",
    "Lobby",
    "BuildStory",
    "GeneratePsoCache",
    "LoadPsoCache",
    "AnalyticsSessionEnd"
};

ScriptExtender::ScriptExtender(ExtenderConfig& config)
    : ThreadedExtenderState(ContextType::Client),
    config_(config)
{
}

ScriptExtender::~ScriptExtender()
{}

void ScriptExtender::Initialize()
{
    // Wrap state change functions even if extension startup failed, otherwise
    // we won't be able to show any startup errors

    auto& lib = GetStaticSymbols();

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());

    if (lib.ecl__GameStateThreaded__GameStateWorker__DoWork != nullptr) {
        gameStateWorkerStart_.Wrap(lib.ecl__GameStateThreaded__GameStateWorker__DoWork);
    }

    if (lib.ecl__GameStateMachine__Update != nullptr) {
        gameStateMachineUpdate_.Wrap(lib.ecl__GameStateMachine__Update);
    }

    DetourTransactionCommit();

    gameStateWorkerStart_.SetWrapper(&ScriptExtender::GameStateWorkerWrapper, this);
    gameStateMachineUpdate_.SetPrePostHook(&ScriptExtender::OnPreUpdate, &ScriptExtender::OnUpdate, this);
    gExtender->GetEngineHooks().ui__deferred__PredicatesManager__UpdateFromUI.SetPreHook(&ScriptExtender::OnUiPredicatesManagerUpdate, this);

    sdl_.EnableHooks();
}

void ScriptExtender::Shutdown()
{
    DEBUG("ecl::ScriptExtender::Shutdown: Exiting");
    sdl_.DisableHooks();
    ContextGuardAnyThread _(ContextType::Client);
    ResetExtensionState();
}

void ScriptExtender::PostStartup()
{
    if (postStartupDone_) return;

    OPTICK_EVENT();
    entityHelpers_.Setup();
    visualHelpers_.Setup();
    gExtender->GetPropertyMapManager().RegisterComponents(entityHelpers_);
    postStartupDone_ = true;
}

bool IsLoadingState(GameState state)
{
    return state == GameState::Init
        || state == GameState::InitMenu
        || state == GameState::InitNetwork
        || state == GameState::InitConnection
        || state == GameState::LoadMenu
        || state == GameState::SwapLevel
        || state == GameState::LoadLevel
        || state == GameState::LoadModule
        || state == GameState::LoadSession
        || state == GameState::UnloadLevel
        || state == GameState::UnloadModule
        || state == GameState::UnloadSession
        || state == GameState::PrepareRunning
        || state == GameState::Installation
        || state == GameState::ModReceiving;
}

// Undo "ModCrashSanityCheck" mechanism that disables mods on an unclean exit
void CleanupSanityCheck()
{
    auto path = GetStaticSymbols().ToPath("ModCrashSanityCheck", PathRootType::UserProfile);
    auto pathw = FromUTF8(path);
    if (PathIsDirectoryW(pathw.c_str())) {
        RemoveDirectoryW(pathw.c_str());
        INFO("Removed ModCrashSanityCheck");
    }
}

void ScriptExtender::OnGameStateChanged(GameState fromState, GameState toState)
{
    startup_timeline::RecordWithCounters(startup_timeline::Side::Client, startup_timeline::What::State,
        (int32_t)fromState, (int32_t)toState);
    // GateKit: the load's trace (the switch LoadTrace).
    load_trace::Role("ecl state");
    load_trace::OnState(1, (int32_t)fromState, (int32_t)toState);
    fast_load::OnClientState((int32_t)fromState, (int32_t)toState);   // GateKit: fast loads
    render_cut::OnClientState((int32_t)fromState, (int32_t)toState);  // GateKit: RenderCutFromRunning
    if ((std::size_t)toState < std::size(GameStateNames)) {
        lua::debug::CoverAtStartState(GameStateNames[(std::size_t)toState]);
    }

    // A save asked for on the command line: the menu's states out of the queue when the game's
    // Init has queued the save's load behind them (the switch DirectSaveLoad).
    if (!gExtender->GetLibraryManager().CriticalInitializationFailed()) {
        direct_save_load::OnGameStateChanged(fromState, toState);
    }

    if (config_.SendCrashReports || !config_.CrashDumpDirectory.empty()) {
        // We need to initialize the crash reporter after the game engine has started,
        // otherwise the game will overwrite the top level exception filter
        InitCrashReporting();
    }

    // Check to make sure that startup is done even if the extender was loaded when the game was already in GameState::Init
    if (toState != GameState::Unknown
        && toState != GameState::StartLoading
        && toState != GameState::InitMenu
        && !gExtender->GetLibraryManager().CriticalInitializationFailed()) {
        // We need to initialize the function library here, as GlobalAllocator isn't available in Init().
        gExtender->PostStartup();
    }

    if (toState == GameState::Menu
        && gExtender->GetLibraryManager().InitializationFailed()) {
        gExtender->PostStartup();
    }

    if ((toState == GameState::InitMenu || toState == GameState::StartLoading)
        && !gExtender->GetLibraryManager().CriticalInitializationFailed()
        && config_.InsanityCheck) {
        CleanupSanityCheck();
    }

#if defined(DEBUG_SERVER_CLIENT)
    DEBUG("ecl::ScriptExtender::OnGameStateChanged(): %s -> %s", 
        GameStateNames[(unsigned)fromState], GameStateNames[(unsigned)toState]);
#endif

    if (fromState != GameState::Unknown) {
        BindToThreadPersistent();
    }

    if (gExtender->WasInitialized() && !gExtender->GetLibraryManager().CriticalInitializationFailed()) {
        if (IsLoadingState(toState)) {
            UpdateClientProgress(EnumInfo<GameState>::Find(toState).GetString());
        } else {
            UpdateClientProgress("");
        }
    }

    switch (fromState) {
    case GameState::LoadModule:
        INFO("ecl::ScriptExtender::OnGameStateChanged(): Loaded module");
        gExtender->GetVirtualTextureHelpers().Load();
        ShowVersionNumber();
        LoadExtensionState(ExtensionStateContext::Game);
        // The modules the game loaded: after a change of the mod list, the hints follow them.
        if (!gExtender->GetLibraryManager().CriticalInitializationFailed()) {
            auto loaded = game_modules::OnModuleLoaded();
            mod_hints::OnModuleLoaded(loaded);
            // Ext.Events.ModulesReloaded: the game loaded another set of modules than at its
            // previous load (not at the first one: there is no previous load to differ from)
            static game_modules::ModuleList previousLoad;
            if (!previousLoad.Empty() && !loaded.SameModules(previousLoad) && extensionState_) {
                ecl::lua::ModulesReloadedEventParams params;
                for (auto const& m : previousLoad.Modules) params.Previous.push_back(m.Uuid);
                for (auto const& m : loaded.Modules) params.Current.push_back(m.Uuid);
                extensionState_->ThrowEvent<ModulesReloadedEvent>(params);
            }
            previousLoad = loaded;
        }
        break;

    // Initialize client state when exiting from a game and returning to menu
    case GameState::LoadMenu:
        LoadExtensionState(ExtensionStateContext::Game);
        break;

    case GameState::LoadSession:
        if (extensionState_) {
            extensionState_->OnGameSessionLoaded();
        }
        break;

    case GameState::InitNetwork:
        network_.ExtendNetworking();
        break;
    }

    switch (toState) {
    case GameState::StartLoading:
        // The machine has made the state current and calls its Enter only after every listener,
        // so the startup playlist can still be rewritten here.
        if (!gExtender->GetLibraryManager().CriticalInitializationFailed()) {
            // The list the game will load, resolved once by the game's own resolver; every early
            // reader below reads it.
            game_modules::OnStartLoading();
            auto client = GetStaticSymbols().GetEoCClient();
            if (client && client->GameStateMachine) {
                startup_videos::OnStartLoading(client->GameStateMachine->CurrentState);
            }
            // The mods' hints, once: the mod list is known and their files reachable, and the
            // loading screen draws its first hint only after this state.
            mod_hints::OnStartLoading();
            // The mods' interface, once: taken here, merged on the interface's next update,
            // before the loading screen's widget is made.
            early_mod_ui::OnStartLoading();
        }
        break;

    case GameState::Init:
        entityHelpers_.Bind();
        ResetExtensionState();
        break;

    case GameState::InitNetwork:
    case GameState::Disconnect:
        network_.Reset();
        break;

    case GameState::UnloadSession:
        INFO("ecl::ScriptExtender::OnGameStateChanged(): Unloading session");
        entityHelpers_.OnDestroy();
        ResetExtensionState();
        break;

    case GameState::Menu:
    {
        auto error = gExtender->GetUpdaterAPI().GetDisplayError();
        if (error) {
            gExtender->GetLibraryManager().ShowStartupError(STDString(*error), false, false);
        } else {
            #if defined(SE_IS_DEVELOPER_BUILD) && defined(NDEBUG)
            if (!config_.DeveloperMode) {
                gExtender->GetLibraryManager().ShowStartupError("This is an experimental version of the Script Extender meant for development use; things may frequently break here. It is recommended to switch back to the Release version unless you know what you are doing!", false, false);
            }
            #endif
        }
        break;
    }

    case GameState::LoadModule:
        gExtender->InitRuntimeLogging();
        break;

    case GameState::LoadSession:
        INFO("ecl::ScriptExtender::OnClientGameStateChanged(): Loading game session");
        LoadExtensionState(ExtensionStateContext::Game);
        network_.ExtendNetworking();
        entityHelpers_.OnInit();
        if (extensionState_) {
            extensionState_->OnGameSessionLoading();
        }
        break;

    case GameState::LoadLevel:
        if (extensionState_) {
            LuaClientPin lua(*extensionState_);
            if (lua) {
                lua->OnLevelLoading();
            }
        }
        break;

    case GameState::UnloadModule:
        stats::gStatStructureCache.Invalidate();
        stats::gStatStructureCache.GetValuePool().Clear();
        break;
    }

    if (extensionState_) {
        ecl::lua::GameStateChangedEventParams params{
            .FromState = fromState,
            .ToState = toState
        };
        extensionState_->ThrowEvent<ClientGameStateChangedEvent>(params);
    }
}

void ScriptExtender::GameStateWorkerWrapper(void (*wrapped)(void*), void* self)
{
    ContextGuard _(ContextType::Client);
    load_trace::Role("ecl worker");   // GateKit: the load's trace
    wrapped(self);
}

#if USE_OPTICK
std::unique_ptr< ::Optick::Event> frameEvent;
#endif

void ScriptExtender::OnPreUpdate(void* self, GameTime* time)
{
#if USE_OPTICK
    static ::Optick::ThreadScope mainThreadScope("Client");
    frameEvent.reset();
    ::Optick::EndFrame();
    ::Optick::Update();

    auto frameNumber = ::Optick::BeginFrame();
    frameEvent = std::make_unique<::Optick::Event>(*::Optick::GetFrameDescription());
    OPTICK_TAG("Frame", frameNumber);
#endif
}

// The start's timeline's rows of the game's own changes, recorded since the last update, thrown to
// the client's Lua as events (on the client's thread, where its Lua runs): Ext.Events.TimelinePlayback,
// LoadingScreen and UIStateChanged. Rows recorded while no Lua state exists are not thrown.
void ScriptExtender::ThrowTimelineEvents()
{
    static uint32_t seen = 0;
    auto count = startup_timeline::Count();
    if (!extensionState_) {
        seen = count;
        return;
    }
    for (; seen < count; seen++) {
        if (!startup_timeline::IsReady(seen)) break;    // filled later, on another thread
        auto const& e = startup_timeline::Get(seen);
        if (e.EventSide != startup_timeline::Side::Client) continue;
        switch (e.Kind) {
        case startup_timeline::What::TimelinePlayback: {
            ecl::lua::TimelinePlaybackEventParams params;
            params.Timeline = e.Name;
            params.Type = e.Parent;
            params.From = e.From;
            params.To = e.To;
            extensionState_->ThrowEvent<TimelinePlaybackEvent>(params);
            break;
        }
        case startup_timeline::What::LoadingScreenShown:
        case startup_timeline::What::LoadingScreenHidden: {
            ecl::lua::LoadingScreenEventParams params;
            params.Visible = e.Kind == startup_timeline::What::LoadingScreenShown;
            extensionState_->ThrowEvent<LoadingScreenEvent>(params);
            break;
        }
        case startup_timeline::What::UIState: {
            ecl::lua::UIStateChangedEventParams params;
            params.State = e.Name;
            params.Parent = e.Parent;
            params.Depth = e.From;
            params.Entered = e.To == 1;
            extensionState_->ThrowEvent<UIStateChangedEvent>(params);
            break;
        }
        default:
            break;
        }
    }
}

void ScriptExtender::OnUpdate(void* self, GameTime* time)
{
    BEGIN_GUARDED()
    load_trace::Role("ecl update");   // GateKit: the load's trace
    render_cut::OnClientTick();       // GateKit: the client's ticks counted (RenderCut.h)
    if (load_trace::Enabled() || fast_load::Any()) {
        auto clientState = GetStaticSymbols().GetClientState();
        bool preparing = clientState && *clientState == GameState::PrepareRunning;
        // the trace reads the poller before a fast load cuts it
        load_trace::OnClientUpdate(self, preparing);
        fast_load::OnClientUpdate(self, preparing);
    }
    startup_timeline::PollClient(self);
    ThrowTimelineEvents();
    // In case we're loaded too late to see LoadModule transition
    BindToThreadPersistent();
    OnUpdateGuarded(self, time);
    END_GUARDED()
}

void ScriptExtender::OnUpdateGuarded(void* self, GameTime* time)
{
    network_.Update();
    RunPendingTasks();
    gExtender->IMGUI().Update();
    if (extensionState_) {
        extensionState_->OnUpdate(*time);
        if (gExtender->GetLuaDebugger()) {
            gExtender->GetLuaDebugger()->ClientTick();
        }
    }

}

void ScriptExtender::OnIncLocalProgress(void* self, int progress, char const* state)
{
    if (strcmp(state, "EffectManager") != 0) {
        UpdateClientProgress(state);
    }
    else {
        UpdateClientProgress("");
    }
}

void ScriptExtender::UpdateServerProgress(STDString const& status)
{
    serverStatus_ = status;
    ShowLoadingProgress();
}

void ScriptExtender::UpdateClientProgress(STDString const& status)
{
    clientStatus_ = status;
    ShowLoadingProgress();
}

void ScriptExtender::ShowLoadingProgress()
{
    // FIXME - not supported for now
}

void ScriptExtender::ShowVersionNumber()
{
    RuntimeStringHandle rsh(FixedString("h5b6e4138g2cf0g4d67gb825gee416cf8c54f"));
    auto versionText = GetStaticSymbols().GetTranslatedStringRepository()->GetTranslatedString(rsh);
    if (versionText && !STDString(*versionText).contains("Script Extender")) {
        auto expandedVersion = STDString(*versionText) +
            "\r\nScript Extender v" + STDString(std::to_string(CurrentVersion)) + " loaded, built on " + BuildDate + ".";
        GetStaticSymbols().GetTranslatedStringRepository()->UpdateTranslatedString(rsh, expandedVersion);
    }
}

void ScriptExtender::OnUiPredicatesManagerUpdate(ui::DeferredPredicatesManager*)
{
    startup_timeline::PollUi();
    // The interface's thread: a reload's composition of the hints goes into the game's list here.
    mod_hints::OnInterfaceThread();

    // Non-threadsafe peek to check if we need to acquire the Lua state lock.
    // (we need to wait for the main thread to finish processing Lua code if we do)
    if (extensionState_ 
        && extensionState_->GetClientLua()
        && extensionState_->GetClientLua()->GetDeferredUIEvents().HasAnyNoesisThreadCallbacks()) {

        ContextGuardAnyThread _(ContextType::Client);
        ecl::LuaClientPin lua(*extensionState_);
        if (lua) {
            lua->GetDeferredUIEvents().PostUpdateNoesis();
        }
    }
}

void ScriptExtender::ResetLuaState()
{
    if (extensionState_ && extensionState_->GetLua()) {
        auto ext = extensionState_.get();

        ext->AddPostResetCallback([&ext]() {
            ext->OnModuleResume();
            auto state = GetStaticSymbols().GetClientState();
            if (state && (state == GameState::Paused || state == GameState::Running)) {
                ext->OnGameSessionLoading();
                ext->OnGameSessionLoaded();
            }
            ext->OnResetCompleted();
        });
        ext->RequestLuaReset(true);
    }
}

void ScriptExtender::ResetExtensionState()
{
    OPTICK_EVENT();
    network_.OnResetExtensionState();
    extensionState_.reset();
    extensionState_ = std::make_unique<ExtensionState>();
    extensionState_->Reset();
    gExtender->ClearPathOverrides();
    extensionLoaded_ = false;
}

void ScriptExtender::LoadExtensionState(ExtensionStateContext ctx)
{
    if (extensionLoaded_ && (!extensionState_ || ctx == extensionState_->Context())) {
        return;
    }

    OPTICK_EVENT(Optick::Category::IO);
    PostStartup();

    if (!extensionState_) {
        ResetExtensionState();
    }

    extensionState_->LoadConfigs();

    if (!gExtender->GetLibraryManager().CriticalInitializationFailed()) {
        OsiMsg("Initializing client with target context " << ContextToString(ctx));
        gExtender->GetLibraryManager().ApplyCodePatches();
        //networkManager_.ExtendNetworkingClient();
        extensionState_->RequestLuaReset(ctx, true);
    }

    extensionLoaded_ = true;
}

END_NS()
