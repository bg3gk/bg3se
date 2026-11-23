#include "stdafx.h"
#include <Extender/ScriptExtender.h>
#include <Extender/Client/ExtensionStateClient.h>
#include <Extender/Server/ExtensionStateServer.h>
#include <Osiris/Debugger/DebugInterface.h>
#include <Lua/Debugger/LuaDebugger.h>
#include <Lua/Debugger/LuaDebugMessages.h>
#include <Extender/Shared/Console.h>
#include <GameDefinitions/Stats/Cache.h>
#include <Extender/Shared/StartupTimeline.h>
#include <Extender/Client/EarlyModUI.h>
#include <Extender/Client/ModHints.h>
#include <Extender/Client/DirectSaveLoad.h>
#include <Extender/Shared/FastLoad.h>
#include "Version.h"
#include "resource.h"
#include <iomanip>
#include <chrono>

#pragma warning(push)
#pragma warning(disable:4267)
#include <Extender/Shared/ExtenderProtocol.pb.h>
#pragma warning(pop)

#include <Extender/Shared/UpdaterAPI.inl>
#include <Extender/Shared/StatLoadOrderHelper.inl>
#include <Extender/Shared/UserVariables.inl>
#include <Extender/Shared/UseActions.inl>
#include <Extender/Shared/VirtualTextures.inl>
#include <Extender/Shared/LoadTrace.inl>
#include <Extender/Shared/RenderCut.inl>   // GateKit: render skip, Lua collector held during a load
#include <Extender/Shared/Headless.inl>    // GateKit: tick waits slept, resource classes stubbed
#if USE_OPTICK
#include <optick_server.h>
#endif

#undef DEBUG_SERVER_CLIENT

namespace bg3se::lua::debug
{
    // Lua/Libs/DebugCover.inl
    void CoverAtStartBegin();
}

namespace bg3se
{

void InitCrashReporting();
void ShutdownCrashReporting();

decltype(ScriptExtender::CoreLibInit)* decltype(ScriptExtender::CoreLibInit)::gHook;
decltype(ScriptExtender::AppUpdatePaths)* decltype(ScriptExtender::AppUpdatePaths)::gHook;
decltype(ScriptExtender::AppLoadGraphicSettings)* decltype(ScriptExtender::AppLoadGraphicSettings)::gHook;
decltype(ScriptExtender::AppInitGame)* decltype(ScriptExtender::AppInitGame)::gHook;
decltype(ScriptExtender::AppInitializeResourceManager)* decltype(ScriptExtender::AppInitializeResourceManager)::gHook;
decltype(ScriptExtender::LoadPackFiles)* decltype(ScriptExtender::LoadPackFiles)::gHook;
decltype(ScriptExtender::LoadFromContextFolder)* decltype(ScriptExtender::LoadFromContextFolder)::gHook;
decltype(ScriptExtender::AdvanceHint)* decltype(ScriptExtender::AdvanceHint)::gHook;
decltype(ScriptExtender::ImageSequenceLoaded)* decltype(ScriptExtender::ImageSequenceLoaded)::gHook;
decltype(ScriptExtender::GameUIUpdate)* decltype(ScriptExtender::GameUIUpdate)::gHook;
decltype(ScriptExtender::ArgumentGetValue)* decltype(ScriptExtender::ArgumentGetValue)::gHook;

std::unique_ptr<ScriptExtender> gExtender;

void LuaDebugThreadRunner(LuaDebugInterface& intf)
{
    intf.Run();
}

ScriptExtender::ScriptExtender()
    : server_(config_),
    client_(config_),
    imgui_(client_.GetSDL())
{
    gCoreLibPlatformInterface.GlobalConsole = new DebugConsole();
}

ScriptExtender::~ScriptExtender()
{}

void ScriptExtender::WarnIfOffline()
{
    // Check if we were loaded through the updaters BG3ScriptExtender.dll postload mechanism.
    // If not, show a note that it's not an auto-updating build
    wchar_t path[1024];
    auto len = GetModuleFileNameW(gCoreLibPlatformInterface.ThisModule, path, std::size(path));
    if (len == 0) return;

    path[len] = 0;
    auto sep = wcsrchr(path, L'\\');
    if (sep == nullptr) return;
    sep++;

    auto end = wcschr(sep, L'.');
    if (end == nullptr) return;
    *end = 0;

    if (_wcsicmp(sep, L"BG3ScriptExtender") != 0) {
        WARN("NOTICE: Script Extender loaded without an updater present; new updates will not be fetched automatically");
    }
}

void ScriptExtender::Initialize()
{
    if (config_.SendCrashReports || !config_.CrashDumpDirectory.empty()) {
        InitCrashReporting();
    }

    startTime_ = GetTickCount64();
    startup_timeline::RecordWithCounters(startup_timeline::Side::Extender, startup_timeline::What::ExtenderLoaded, -1, -1);

    // GateKit: the load's trace, measuring only (the switch LoadTrace, off by default).
    if (config_.LoadTrace) {
        load_trace::Install(config_.LoadTraceSampleMs);
    }
    // GateKit: fast loads for automated runs (each switch off by default).
    fast_load::Configure(config_.FastLoadClientFrames, config_.FastLoadServerTick,
        config_.FastLoadStreamingWait, config_.FastLoadFades, config_.FastLoadStreamingGate);

    if (Libraries.GetGameVersion(gameVersion_)) {
        if (gameVersion_.IsSupported()) {
            INFO("Game version v%d.%d.%d.%d OK", gameVersion_.Major, gameVersion_.Minor, gameVersion_.Revision, gameVersion_.Build);
        } else {
            ERR("Game version v%d.%d.%d.%d is not supported, please upgrade!", gameVersion_.Major, gameVersion_.Minor, gameVersion_.Revision, gameVersion_.Build);
            // Hard exit below a certain version as the EoCClient error display UI won't work anymore
            Fail("Script Extender doesn't support game versions below v4.68, please upgrade!");
        }
    } else {
        ERR("Failed to retrieve game version info.");
    }

    WarnIfOffline();

    DEBUG("ScriptExtender::Initialize: Starting");
    auto initStart = std::chrono::high_resolution_clock::now();

    InitStaticSymbols();

    if (!Libraries.FindLibraries(gameVersion_.Revision)) {
        ERR("ScriptExtender::Initialize: Could not load libraries; skipping scripting extension initialization.");
    }

    if (!Libraries.CriticalInitializationFailed()) {
        imgui_.EnableHooks();
        updaterApi_.MarkReadyToDisplayErrors();
    }

    server_.Initialize();
    client_.Initialize();

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());

    if (GetStaticSymbols().App__Ctor != nullptr) {
        CoreLibInit.Wrap(GetStaticSymbols().App__Ctor);
        CoreLibInit.SetPreHook(&ScriptExtender::OnCoreLibInit, this);
    }

    if (GetStaticSymbols().App__UpdatePaths != nullptr) {
        AppUpdatePaths.Wrap(GetStaticSymbols().App__UpdatePaths);
        AppUpdatePaths.SetPostHook(&ScriptExtender::OnAppUpdatePaths, this);
    }

    if (GetStaticSymbols().App__LoadGraphicSettings != nullptr) {
        AppLoadGraphicSettings.Wrap(GetStaticSymbols().App__LoadGraphicSettings);
        AppLoadGraphicSettings.SetPostHook(&ScriptExtender::OnAppLoadGraphicSettings, this);
    }

    // The engine's start steps the timeline marks (no other use).
    if (GetStaticSymbols().App__InitGame != nullptr) {
        AppInitGame.Wrap(GetStaticSymbols().App__InitGame);
        AppInitGame.SetPrePostHook(&ScriptExtender::OnAppInitGameBegin, &ScriptExtender::OnAppInitGameEnd, this);
    }

    if (GetStaticSymbols().App__InitializeResourceManager != nullptr) {
        AppInitializeResourceManager.Wrap(GetStaticSymbols().App__InitializeResourceManager);
        AppInitializeResourceManager.SetPrePostHook(&ScriptExtender::OnInitializeResourceManagerBegin,
            &ScriptExtender::OnInitializeResourceManagerEnd, this);
    }

    if (GetStaticSymbols().ls__PackedFileManager__LoadPackFiles != nullptr) {
        LoadPackFiles.Wrap(GetStaticSymbols().ls__PackedFileManager__LoadPackFiles);
        LoadPackFiles.SetPrePostHook(&ScriptExtender::OnLoadPackFilesBegin, &ScriptExtender::OnLoadPackFilesEnd, this);
    }

    if (GetStaticSymbols().ls__TranslatedStringRepository__LoadFromContextFolder != nullptr) {
        LoadFromContextFolder.Wrap(GetStaticSymbols().ls__TranslatedStringRepository__LoadFromContextFolder);
        LoadFromContextFolder.SetPrePostHook(&ScriptExtender::OnLoadFromContextFolderBegin,
            &ScriptExtender::OnLoadFromContextFolderEnd, this);
    }

    if (GetStaticSymbols().gui__DCLoadingScreen__OnAdvanceHintCommand != nullptr) {
        AdvanceHint.Wrap(GetStaticSymbols().gui__DCLoadingScreen__OnAdvanceHintCommand);
        AdvanceHint.SetPrePostHook(&ScriptExtender::OnAdvanceHintBegin, &ScriptExtender::OnAdvanceHintEnd, this);
    }

    if (GetStaticSymbols().gui__LSImageSequence__OnLoaded != nullptr) {
        ImageSequenceLoaded.Wrap(GetStaticSymbols().gui__LSImageSequence__OnLoaded);
        ImageSequenceLoaded.SetPostHook(&ScriptExtender::OnImageSequenceLoaded, this);
    }

    if (config_.EarlyModInterface && GetStaticSymbols().gui__GameUI__UpdateFromUI != nullptr) {
        GameUIUpdate.Wrap(GetStaticSymbols().gui__GameUI__UpdateFromUI);
        GameUIUpdate.SetPreHook(&ScriptExtender::OnGameUIUpdate, this);
    }

    if (config_.WideCommandLine && GetStaticSymbols().ls__ArgumentString__GetValue != nullptr) {
        ArgumentGetValue.Wrap(GetStaticSymbols().ls__ArgumentString__GetValue);
        ArgumentGetValue.SetPostHook(&ScriptExtender::OnArgumentGetValue, this);
    }

    // GateKit: the render thread's frame and the game's Lua collector (RenderCut.h), each only when
    // its switch is on (off by default).
    render_cut::Install(config_.RenderCut, config_.RenderCutSkip, config_.FastLoadLuaGC, config_.RenderCutFromRunning,
        config_.RenderCutSkipPaceUs, config_.RenderCutSkipPaceStartOnly);
    // GateKit: the tick waits slept and resource classes stubbed (Headless.h), off by default.
    headless::Install(config_.IdleSleep, config_.StubFileExtensions);
    // GateKit: the treasure generation's wrapper, inert until pinned (Headless.h), off by default.
    headless::InstallTreasurePin(config_.TreasurePin);
    // GateKit (W3 round 7), measuring: VirtualAlloc calls of at least N MB named by their callers.
    headless::InstallLargeAllocTrace(config_.LargeAllocTraceMB);
    // Record self-termination when exit diagnostics are enabled.
    headless::InstallExitWitness(config_.ExitWitness, config_.LogDirectory);

    DetourTransactionCommit();

#if USE_OPTICK
    if (config_.Optick) {
        ::Optick::Server::Enable();
        if (::Optick::Server::Get().IsListening()) {
            DEBUG("Optick profiler listening on port %d", ::Optick::Server::Get().GetPort());
        } else {
            ERR("Optick profiler startup failed");
        }
    }
#endif

    if (config_.EnableLuaDebugger && luaDebuggerThread_ == nullptr) {
        DEBUG("Starting Lua debugger server");
        try {
            luaDebugInterface_ = std::make_unique<LuaDebugInterface>(config_.LuaDebuggerPort);
            luaDebugMsgHandler_ = std::make_unique<lua::dbg::DebugMessageHandler>(std::ref(*luaDebugInterface_));
            luaDebuggerThread_ = new std::thread(std::bind(LuaDebugThreadRunner, std::ref(*luaDebugInterface_)));
            luaDebugger_ = std::make_unique<lua::dbg::Debugger>(std::ref(*luaDebugMsgHandler_));
            DEBUG("Lua debugger listening on 127.0.0.1:%d; DBG protocol version %d",
                config_.LuaDebuggerPort, lua::dbg::DebugMessageHandler::ProtocolVersion);
        } catch (std::exception& e) {
            ERR("Lua debugger startup failed: %s", e.what());
        }
    }

    auto initEnd = std::chrono::high_resolution_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(initEnd - initStart).count();
    DEBUG("Library startup took %lld ms", ms);
    startup_timeline::RecordStep("ScriptExtender::Initialize", startup_timeline::StepEnd);
    // the extender's own hooks are in place: the start's coverage counters go on now, before the engine starts
    lua::debug::CoverAtStartBegin();

    auto app = GetStaticSymbols().AppInstance;
    if (app && *app) {
        DEBUG("We're already past App load, triggering CoreLibInit");
        OnCoreLibInit(nullptr);
    }

    if (config_.SendCrashReports || !config_.CrashDumpDirectory.empty()) {
        ShutdownCrashReporting();
    }
}

void ScriptExtender::Shutdown()
{
    DEBUG("ScriptExtender::Shutdown: Exiting");
    server_.Shutdown();
    client_.Shutdown();
    engineHooks_.UnhookAll();
}

void ScriptExtender::LogLuaError(std::string_view msg)
{
    gCoreLibPlatformInterface.GlobalConsole->Print(DebugMessageType::Error, msg.data());
    server_.Osiris().LogError(msg);
}

void ScriptExtender::LogOsirisError(std::string_view msg)
{
    gCoreLibPlatformInterface.GlobalConsole->Print(DebugMessageType::Error, msg.data());
    server_.Osiris().LogError(msg);

    if (luaDebugger_) {
        luaDebugger_->OnGenericError(msg.data());
    }
}

void ScriptExtender::LogOsirisWarning(std::string_view msg)
{
    gCoreLibPlatformInterface.GlobalConsole->Print(DebugMessageType::Warning, msg.data());
    server_.Osiris().LogWarning(msg);
}

void ScriptExtender::LogOsirisMsg(std::string_view msg)
{
    gCoreLibPlatformInterface.GlobalConsole->Print(DebugMessageType::Osiris, msg.data());
    server_.Osiris().LogMessage(msg);
}

std::wstring ScriptExtender::MakeLogFilePath(std::wstring const & Type, std::wstring const & Extension)
{
    if (config_.LogDirectory.empty()) {
        config_.LogDirectory = FromUTF8(GetStaticSymbols().ToPath("/Script Extender Logs", PathRootType::UserProfile));
    }

    if (config_.LogDirectory.empty()) {
        return L"";
    }

    BOOL created = CreateDirectoryW(config_.LogDirectory.c_str(), NULL);
    if (created == FALSE) {
        DWORD lastError = GetLastError();
        if (lastError != ERROR_ALREADY_EXISTS) {
            std::wstringstream err;
            err << L"Could not create log directory '" << config_.LogDirectory << "': Error " << lastError;
            return L"";
        }
    }

    auto now = std::chrono::system_clock::now();
    auto tt = std::chrono::system_clock::to_time_t(now);
    std::tm tm;
    gmtime_s(&tm, &tt);

    std::wstringstream ss;
    ss << config_.LogDirectory << L"\\"
        << Type << L" "
        << std::setfill(L'0')
        << (tm.tm_year + 1900) << L"-"
        << std::setw(2) << (tm.tm_mon + 1) << L"-"
        << std::setw(2) << tm.tm_mday << L" "
        << std::setw(2) << tm.tm_hour << L"-"
        << std::setw(2) << tm.tm_min << L"-"
        << std::setw(2) << tm.tm_sec << L"." << Extension;
    return ss.str();
}

void ScriptExtender::OnStatsLoadGuarded(stats::RPGStats__LoadProc* wrapped, stats::RPGStats* mgr, Array<STDString>* paths)
{
    // Stats load is scheduled from the client on the shared worker pool
    ContextGuard _(ContextType::Client);

    // Ensure that we have an empty extension state ready in case OnStatsLoad() is the first callback
    // due to late load
    if (!client_.HasExtensionState()) {
        client_.ResetExtensionState();
    }

    statLoadOrderHelper_.OnLoadStarted();
    client_.LoadExtensionState(ExtensionStateContext::Load);

    lua::EmptyEventParams evt;
    client_.GetExtensionState().ThrowEvent<ModuleLoadStartedEvent>(evt);

    {
        DisableCrashReporting _;
        wrapped(mgr, paths);
    }

    stats::gStatStructureCache.OnStatsLoaded();
    client_.LoadExtensionState(ExtensionStateContext::Game);

    if (client_.HasExtensionState()) {
        client_.GetExtensionState().OnStatsLoaded();
    }

    imgui_.EnableUI(true);
}

void ScriptExtender::OnStatsLoad(stats::RPGStats__LoadProc* wrapped, stats::RPGStats* mgr, Array<STDString>* paths)
{
    BEGIN_GUARDED()
    OnStatsLoadGuarded(wrapped, mgr, paths);
    END_GUARDED()
}

void ScriptExtender::OnECSUpdate(ecs::EntityWorld__UpdateProc* wrapped, ecs::EntityWorld* entityWorld, GameTime const& time)
{
    BEGIN_GUARDED()
    OnECSUpdateGuarded(wrapped, entityWorld, time);
    END_GUARDED()
}

void ScriptExtender::OnECSUpdateGuarded(ecs::EntityWorld__UpdateProc* wrapped, ecs::EntityWorld* entityWorld, GameTime const& time)
{
    auto ecs = GetECS(entityWorld);
    if (ecs != nullptr) {
        ecs->PreUpdate();
        
        {
            DisableCrashReporting _;
            wrapped(entityWorld, time);
        }

        ecs->PostUpdate();

        auto ctx = GetCurrentContextType();
        se_assert(ctx != ContextType::None);

        if (ctx == ContextType::Server) {
            if (GetServer().HasExtensionState()) {
                esv::LuaServerPin lua(GetServer().GetExtensionState());
                if (lua) {
                    lua->GetComponentEventHooks().FireDeferredEvents();
                    lua->GetSystemEventHooks().FireDeferredEvents();
                    lua->GetServerReplicationEventHooks().OnEntityReplication(*entityWorld, entityWorld->Replication);
                }
            }
        } else {
            if (GetClient().HasExtensionState()) {
                ecl::LuaClientPin lua(GetClient().GetExtensionState());
                if (lua) {
                    lua->GetComponentEventHooks().FireDeferredEvents();
                    lua->GetSystemEventHooks().FireDeferredEvents();
                }
            }
        }
    } else {
        DisableCrashReporting _;
        wrapped(entityWorld, time);
    }
}

void ScriptExtender::OnECSFlushECBs(ecs::EntityWorld* entityWorld)
{
    BEGIN_GUARDED()
    auto ecs = GetECS(entityWorld);
    if (ecs != nullptr) {
        ecs->OnFlushECBs();
    }
    END_GUARDED()
}

void ScriptExtender::OnFindPath(AiGrid* self, AiPathId pathId)
{
    auto ctx = GetCurrentContextType();

    if (ctx == ContextType::Server) {
        if (server_.HasExtensionState()) {
            esv::LuaServerPin lua(GetServer().GetExtensionState());
            if (lua) {
                lua->OnFindPath(self, pathId);
            }
        }
    } else if (ctx == ContextType::Client) {
        if (GetClient().HasExtensionState()) {
            ecl::LuaClientPin lua(GetClient().GetExtensionState());
            if (lua) {
                lua->OnFindPath(self, pathId);
            }
        }
    }
}

bool ScriptExtender::HasFeatureFlag(char const * flag) const
{
    return (server_.HasExtensionState() && server_.GetExtensionState().HasFeatureFlag(flag))
        || (client_.HasExtensionState() && client_.GetExtensionState().HasFeatureFlag(flag));
}

ExtensionStateBase* ScriptExtender::GetCurrentExtensionState()
{
    auto ctx = GetCurrentContextType();

    if (ctx == ContextType::Server) {
        if (server_.HasExtensionState()) {
            return &server_.GetExtensionState();
        } else {
            return nullptr;
        }
    } else if (ctx == ContextType::Client) {
        if (client_.HasExtensionState()) {
            return &client_.GetExtensionState();
        } else {
            ERR("GetCurrentExtensionState() called from client thread %d, but no client state is available!", GetCurrentThreadId());
            return nullptr;
        }
    } else {
        ERR("GetCurrentExtensionState() called from thread %d that is not bound to any context!", GetCurrentThreadId());
        if (client_.HasExtensionState()) {
            return &client_.GetExtensionState();
        } else {
            return nullptr;
        }
    }
}

ecs::EntitySystemHelpersBase* ScriptExtender::GetECS(ecs::EntityWorld* world)
{
    if (client_.GetEntityHelpers().HasEntityWorld() && world == client_.GetEntityHelpers().GetEntityWorld()) {
        return &client_.GetEntityHelpers();
    } else if (server_.GetEntityHelpers().HasEntityWorld() && world == server_.GetEntityHelpers().GetEntityWorld()) {
        return &server_.GetEntityHelpers();
    } else {
        // This can happen if we're not yet past server/client setup phase
        return nullptr;
    }
}

void ScriptExtender::OnCoreLibInit(void * self)
{
    // The pre-hook of the symbol App__Ctor, which is BaseApp::BaseApp (the App's base constructor);
    // called with no object when the App existed before the extender's Initialize.
    startup_timeline::RecordStep(self != nullptr ? "BaseApp::BaseApp" : "BaseApp::BaseApp (before the extender)",
        startup_timeline::StepAt);
    PostStartup();
}

void ScriptExtender::OnAppInitGameBegin(App* self)
{
    startup_timeline::RecordStep("App::InitGame", startup_timeline::StepBegin);
}

void ScriptExtender::OnAppInitGameEnd(App* self, bool result)
{
    startup_timeline::RecordStep("App::InitGame", startup_timeline::StepEnd);
}

void ScriptExtender::OnInitializeResourceManagerBegin(App* self)
{
    startup_timeline::RecordStep("App::InitializeResourceManager", startup_timeline::StepBegin);
}

void ScriptExtender::OnInitializeResourceManagerEnd(App* self)
{
    startup_timeline::RecordStep("App::InitializeResourceManager", startup_timeline::StepEnd);
}

void ScriptExtender::OnLoadPackFilesBegin(void* self, bool a1, bool a2)
{
    startup_timeline::RecordStep("ls::PackedFileManager::LoadPackFiles", startup_timeline::StepBegin);
}

void ScriptExtender::OnLoadPackFilesEnd(void* self, bool a1, bool a2)
{
    startup_timeline::RecordStep("ls::PackedFileManager::LoadPackFiles", startup_timeline::StepEnd);
}

void ScriptExtender::OnLoadFromContextFolderBegin(void* self, Path const* folder, bool fallback, bool a3)
{
    startup_timeline::RecordStep("ls::TranslatedStringRepository::LoadFromContextFolder", startup_timeline::StepBegin);
}

void ScriptExtender::OnLoadFromContextFolderEnd(void* self, Path const* folder, bool fallback, bool a3, bool result)
{
    startup_timeline::RecordStep("ls::TranslatedStringRepository::LoadFromContextFolder", startup_timeline::StepEnd);
}

void ScriptExtender::OnAdvanceHintBegin(void* self)
{
    // A composition of the hints made at a reload goes into the list before this draw reads it.
    ecl::mod_hints::OnInterfaceThread();
    startup_timeline::BeforeAdvanceHint(self);
}

void ScriptExtender::OnAdvanceHintEnd(void* self)
{
    startup_timeline::AfterAdvanceHint(self);
}

void ScriptExtender::OnImageSequenceLoaded(void* self, void* a1, void* a2)
{
    startup_timeline::AfterImageSequenceLoaded(self);
}

void ScriptExtender::OnGameUIUpdate(void* self, void* time)
{
    ecl::early_mod_ui::BeforeGameUIUpdate(self);
}

void ScriptExtender::OnArgumentGetValue(void* self, char const* key, STDString* value, bool found)
{
    ecl::direct_save_load::OnArgumentValue(key, value, found);
}

void ScriptExtender::OnAppUpdatePaths(App* self)
{
    startup_timeline::RecordStep("App::UpdatePaths", startup_timeline::StepEnd);
    if (!config_.CustomProfile.empty() && GetStaticSymbols().ls__PathRoots) {
        auto& gameLocalPath = GetStaticSymbols().ls__PathRoots[4];
        if (gameLocalPath->ends_with("Baldur's Gate 3")) {
            *gameLocalPath = gameLocalPath->substr(0, gameLocalPath->size() - 15) + config_.CustomProfile.c_str();
        }
    }
}

class ClientEventManagerHook
{
public:
    struct GameStates
    {
        ecl::GameState From;
        ecl::GameState To;
    };

    virtual ~ClientEventManagerHook()
    {}

    virtual bool OnGameStateChanged(GameStates& states)
    {
        BEGIN_GUARDED()
        gExtender->GetClient().OnGameStateChanged(states.From, states.To);
        END_GUARDED()

        return true;
    }

    virtual bool Unknown()
    {
        return false;
    }

    uint64_t dummy{ 0 };
};

class ServerEventManagerHook
{
public:
    struct GameStates
    {
        esv::GameState From;
        esv::GameState To;
    };

    virtual ~ServerEventManagerHook()
    {}

    virtual bool OnGameStateChanged(GameStates& states)
    {
        BEGIN_GUARDED()
        gExtender->GetServer().OnGameStateChanged(states.From, states.To);
        END_GUARDED()

        return true;
    }

    virtual bool Unknown()
    {
        return false;
    }

    uint64_t dummy{ 0 };
};

void ScriptExtender::OnAppLoadGraphicSettings(App * self)
{
    // The extender's App__LoadGraphicSettings: slot +0xA8 of App's vtable, the Toolkit's
    // App::LoadAndResaveGraphicsSettings (it reads graphicSettings.lsx).
    startup_timeline::RecordStep("App::LoadAndResaveGraphicsSettings", startup_timeline::StepEnd);
    HookStateMachineUpdates();
}

void ScriptExtender::HookStateMachineUpdates()
{
    if (updateHooksAdded_ || Libraries.CriticalInitializationFailed()) return;

    auto clientEvtMgr = GetStaticSymbols().ecl__gGameStateEventManager;
    if (clientEvtMgr && *clientEvtMgr) {
        auto client = GameAlloc<ClientEventManagerHook>();
        (*clientEvtMgr)->Callbacks.push_back(&client->dummy);
    }

    auto serverEvtMgr = GetStaticSymbols().esv__gGameStateEventManager;
    if (serverEvtMgr && *serverEvtMgr) {
        auto server = GameAlloc<ServerEventManagerHook>();
        (*serverEvtMgr)->Callbacks.push_back(&server->dummy);
    }

    updateHooksAdded_ = true;
}

void ScriptExtender::ClearPathOverrides()
{
    std::unique_lock lock(pathOverrideMutex_);
    pathOverrides_.clear();
}


void ScriptExtender::AddPathOverride(STDString const & path, STDString const & overriddenPath)
{
    auto absolutePath = GetStaticSymbols().ToPath(path, PathRootType::Data);
    auto absoluteOverriddenPath = GetStaticSymbols().ToPath(overriddenPath, PathRootType::Data);

    std::unique_lock lock(pathOverrideMutex_);
    pathOverrides_.insert(std::make_pair(absolutePath, absoluteOverriddenPath));
}

std::optional<STDString> ScriptExtender::GetPathOverride(STDString const & path)
{
    auto absolutePath = GetStaticSymbols().ToPath(path, PathRootType::Data);

    std::unique_lock lock(pathOverrideMutex_);
    auto it = pathOverrides_.find(absolutePath);
    if (it != pathOverrides_.end()) {
        return it->second;
    } else {
        return {};
    }
}

FileReader * ScriptExtender::OnFileReaderCreate(FileReader__CtorProc* next, FileReader * self, Path const& path, unsigned int type, unsigned int unknown)
{
    startup_timeline::FileOpened(path.Name.data(), path.Name.size());

    if (!pathOverrides_.empty()) {
        std::shared_lock lock(pathOverrideMutex_);
        auto it = pathOverrides_.find(path.Name);
        if (it != pathOverrides_.end()) {
            DEBUG("FileReader path override: %s -> %s", path.Name.c_str(), it->second.c_str());
            Path overriddenPath;
            overriddenPath.Name = it->second;
#if !defined(OSI_EOCAPP)
            overriddenPath.Unknown = path->Unknown;
#endif
            lock.unlock();
            return next(self, overriddenPath, type, unknown);
        }
    }

    if (path.Name.size() > 4 
        && memcmp(path.Name.data() + path.Name.size() - 4, ".txt", 4) == 0) {
        statLoadOrderHelper_.OnStatFileOpened(path);
    }

    // GateKit: a stubbed resource class (the switches StubFileExtensions and StubFileTarget,
    // Headless.h): the open gets the stand-in file, or finds no file, as for a missing one.
    if (headless::Stubbed(path.Name.data(), path.Name.size())
        && (config_.StubFileTarget.empty() || path.Name.find(config_.StubFileTarget.c_str()) == STDString::npos)) {
        Path stub;
        if (config_.StubFileTarget.empty()) {
            stub.Name = path.Name;
            stub.Name += ".gatekit-stub";
        } else {
            stub.Name = config_.StubFileTarget.c_str();
        }
#if !defined(OSI_EOCAPP)
        stub.Unknown = path->Unknown;
#endif
        return next(self, stub, type, unknown);
    }

    DisableCrashReporting _;
    // GateKit: the load's trace (the switch LoadTrace): the open's time, size and thread.
    if (load_trace::Enabled()) {
        auto began = load_trace::FileReaderBegin();
        auto reader = next(self, path, type, unknown);
        load_trace::FileReaderEnd(began, path.Name.data(), path.Name.size(), type, unknown, self->FileSize, self->IsLoaded);
        return reader;
    }
    return next(self, path, type, unknown);
}

void ScriptExtender::PostStartup()
{
    if (postStartupDone_) return;

    std::lock_guard _(globalStateLock_);
    startup_timeline::RecordStep("ScriptExtender::PostStartup", startup_timeline::StepBegin);
    // We need to initialize the function library here, as GlobalAllocator isn't available in Init().
    if (Libraries.PostStartupFindLibraries()) {
        lua::InitObjectProxyPropertyMaps();
        lua::InitEventRegistry();
        TypeInformationRepository::GetInstance().Initialize();
        lua::RegisterLibraries();
        TypeInformationRepository::GetInstance().Finalize();

        // Jank workaround to game bug where the reference count for FixedString 0 (the "Cast" key)
        // gets erroneously decremented during module load.
        FixedString cast("Cast");
        const_cast<FixedStringBase::Header*>(cast.GetMetadata())->RefCount = 100000;

        engineHooks_.HookAll();
        hooks_.Startup();
        server_.PostStartup();
        client_.PostStartup();

        luaBuiltinBundle_.SetResourcePath(config_.LuaBuiltinResourceDirectory);
        if (!luaBuiltinBundle_.LoadBuiltinResource(IDR_LUA_BUILTIN_BUNDLE)) {
            ERR("Failed to load Lua builtin resource bundle!");
        }

        engineHooks_.FileReader__ctor.SetWrapper(&ScriptExtender::OnFileReaderCreate, this);
        //engineHooks_.Kernel_FindFirstFileW.SetWrapper(&ScriptExtender::OnFindFirstFileW, this);
        //engineHooks_.Kernel_FindNextFileW.SetWrapper(&ScriptExtender::OnFindNextFileW, this);
        //engineHooks_.Kernel_FindClose.SetWrapper(&ScriptExtender::OnFindClose, this);
        
        engineHooks_.RPGStats__Load.SetWrapper(&ScriptExtender::OnStatsLoad, this);
        engineHooks_.ecs__EntityWorld__Update.SetWrapper(&ScriptExtender::OnECSUpdate, this);
        engineHooks_.ecs__EntityWorld__FlushECBs.SetPreHook(&ScriptExtender::OnECSFlushECBs, this);
        engineHooks_.eoc__AiGrid__FindPath.SetPreHook(&ScriptExtender::OnFindPath, this);
    }

    GameVersionInfo gameVersion;
    if (Libraries.GetGameVersion(gameVersion) && !gameVersion.IsSupported()) {
        std::stringstream ss;
        ss << "Your game version (v" << gameVersion.Major << "." << gameVersion.Minor << "." << gameVersion.Revision << "." << gameVersion.Build
            << ") is not supported by the Script Extender";
        Libraries.ShowStartupError(ss.str().c_str(), true, false);
    } else if (Libraries.CriticalInitializationFailed()) {
        Libraries.ShowStartupError("A severe error has occurred during Script Extender initialization. Extension features will be unavailable.", true, false);
    } else if (Libraries.InitializationFailed()) {
        Libraries.ShowStartupError("An error has occurred during Script Extender initialization. Some extension features might be unavailable.", true, false);
    }

    HookStateMachineUpdates();

    postStartupDone_ = true;
    startup_timeline::RecordStep("ScriptExtender::PostStartup", startup_timeline::StepEnd);
}

void ScriptExtender::InitRuntimeLogging()
{
    if (!config_.LogRuntime) return;

    auto path = MakeLogFilePath(L"Extender Runtime", L"log");
    gCoreLibPlatformInterface.GlobalConsole->OpenLogFile(path);
    DEBUG("Extender runtime log written to '%s'", ToStdUTF8(path).c_str());
}

void ScriptExtender::OnSDLEvent(SDL_Event* event)
{
    if (event->type == SDL_KEYDOWN 
        && event->key.keysym.scancode == SDL_SCANCODE_F12 
        && (unsigned)((SDLKeyModifier)event->key.keysym.mod & (SDLKeyModifier::LCtrl | SDLKeyModifier::RCtrl)) != 0) {
        
        auto console = gCoreLibPlatformInterface.GlobalConsole;
        if (!console->WasCreated()) {
            console->Create();
        }

    }
}

}
