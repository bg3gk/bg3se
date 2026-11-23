#pragma once

#include <Extender/Shared/ExtenderConfig.h>
#include <GameDefinitions/Osiris.h>
#include <GameDefinitions/GuidResources.h>
#include <Extender/Client/ScriptExtenderClient.h>
#include <Extender/Client/IMGUI/IMGUI.h>
#include <Extender/Server/ScriptExtenderServer.h>
#include <Extender/Shared/StatLoadOrderHelper.h>
#include <Extender/Shared/VirtualTextures.h>
#include <Extender/Shared/UseActions.h>
#include <Extender/Shared/Hooks.h>
#include <Extender/Shared/UpdaterAPI.h>
#include <Lua/Shared/LuaBundle.h>
#include <Lua/Shared/Proxies/LuaCppPropertyMapManager.h>
#include <GameHooks/OsirisWrappers.h>
#include <GameHooks/DataLibraries.h>
#include <GameHooks/EngineHooks.h>

#include <thread>
#include <mutex>
#include <shared_mutex>

namespace bg3se {


class ScriptExtender
{
public:
    ScriptExtender();
    ~ScriptExtender();

    void Initialize();
    void PostStartup();
    void Shutdown();

    inline bool WasInitialized() const
    {
        return postStartupDone_;
    }

    inline ExtenderConfig & GetConfig()
    {
        return config_;
    }

    void LogLuaError(std::string_view msg);
    void LogOsirisError(std::string_view msg);
    void LogOsirisWarning(std::string_view msg);
    void LogOsirisMsg(std::string_view msg);

    inline esv::ScriptExtender& GetServer()
    {
        return server_;
    }

    inline ecl::ScriptExtender& GetClient()
    {
        return client_;
    }

    inline LibraryManager const & GetLibraryManager() const
    {
        return Libraries;
    }

    inline LibraryManager & GetLibraryManager()
    {
        return Libraries;
    }

    inline lua::dbg::Debugger* GetLuaDebugger()
    {
        return luaDebugger_.get();
    }

    inline lua::dbg::DebugMessageHandler* GetLuaDebugMessageHandler()
    {
        return luaDebugMsgHandler_.get();
    }

    ExtensionStateBase* GetCurrentExtensionState();
    ecs::EntitySystemHelpersBase* GetECS(ecs::EntityWorld* world);

    bool HasFeatureFlag(char const *) const;

    inline stats::StatLoadOrderHelper& GetStatLoadOrderHelper()
    {
        return statLoadOrderHelper_;
    }

    inline EngineHooks& GetEngineHooks()
    {
        return engineHooks_;
    }

    inline Hooks& GetHooks()
    {
        return hooks_;
    }

    inline lua::LuaBundle& GetLuaBuiltinBundle()
    {
        return luaBuiltinBundle_;
    }

    inline lua::CppPropertyMapManager& GetPropertyMapManager()
    {
        return propertyMapManager_;
    }

    inline VirtualTextureHelpers& GetVirtualTextureHelpers()
    {
        return virtualTextures_;
    }

    inline UseActionHelpers& GetUseActionHelpers()
    {
        return useActions_;
    }

    inline GameVersionInfo const& GetGameVersion() const
    {
        return gameVersion_;
    }

    inline ULONGLONG GetStartTime() const
    {
        return startTime_;
    }

    inline extui::IMGUIManager& IMGUI()
    {
        return imgui_;
    }

    inline UpdaterAPIHelpers& GetUpdaterAPI()
    {
        return updaterApi_;
    }

    inline uint32_t CreateGenerationId()
    {
        return nextGenerationId_.fetch_add(1);
    }

    void ClearPathOverrides();
    void AddPathOverride(STDString const & path, STDString const & overriddenPath);
    std::optional<STDString> GetPathOverride(STDString const& path);

    std::wstring MakeLogFilePath(std::wstring const& Type, std::wstring const& Extension);
    void InitRuntimeLogging();

    void OnSDLEvent(SDL_Event* event);

    enum class CoreLibInitTag {};
    WrappableFunction<CoreLibInitTag, CoreLibInitProc> CoreLibInit;
    enum class AppUpdatePathsTag {};
    WrappableFunction<AppUpdatePathsTag, App__UpdatePathsProc> AppUpdatePaths;
    enum class AppLoadGraphicSettingsTag {};
    WrappableFunction<AppLoadGraphicSettingsTag, App__LoadGraphicSettingsProc> AppLoadGraphicSettings;
    // Steps of the engine's start, hooked only to mark them in the start's timeline.
    enum class AppInitGameTag {};
    WrappableFunction<AppInitGameTag, App__InitGameProc> AppInitGame;
    enum class AppInitializeResourceManagerTag {};
    WrappableFunction<AppInitializeResourceManagerTag, App__InitializeResourceManagerProc> AppInitializeResourceManager;
    enum class LoadPackFilesTag {};
    WrappableFunction<LoadPackFilesTag, PackedFileManager__LoadPackFilesProc> LoadPackFiles;
    enum class LoadFromContextFolderTag {};
    WrappableFunction<LoadFromContextFolderTag, TranslatedStringRepository__LoadFromContextFolderProc> LoadFromContextFolder;
    // The loading screen's draw of a hint, wrapped only for the timeline's HintShown rows.
    enum class AdvanceHintTag {};
    WrappableFunction<AdvanceHintTag, DCLoadingScreen__OnAdvanceHintCommandProc> AdvanceHint;
    // The loading screen's slideshow made ready, wrapped only for the timeline's WallpaperSet rows.
    enum class ImageSequenceLoadedTag {};
    WrappableFunction<ImageSequenceLoadedTag, LSImageSequence__OnLoadedProc> ImageSequenceLoaded;
    // The interface's per-frame update: the mods' interface merged before the first loading screen
    // (Extender/Client/EarlyModUI.h).
    enum class GameUIUpdateTag {};
    WrappableFunction<GameUIUpdateTag, GameUI__UpdateFromUIProc> GameUIUpdate;
    // The game's read of a command-line argument's value: given in UTF-8 from the wide command
    // line (Extender/Client/DirectSaveLoad.h, the switch WideCommandLine).
    enum class ArgumentGetValueTag {};
    WrappableFunction<ArgumentGetValueTag, ArgumentString__GetValueProc> ArgumentGetValue;

private:
    esv::ScriptExtender server_;
    ecl::ScriptExtender client_;
    LibraryManager Libraries;
    EngineHooks engineHooks_;
    Hooks hooks_;
    bool LibrariesPostInitialized{ false };
    std::recursive_mutex globalStateLock_;
    std::shared_mutex pathOverrideMutex_;
    std::unordered_map<STDString, STDString> pathOverrides_;
    stats::StatLoadOrderHelper statLoadOrderHelper_;
    lua::LuaBundle luaBuiltinBundle_;
    lua::CppPropertyMapManager propertyMapManager_;
    VirtualTextureHelpers virtualTextures_;
    UseActionHelpers useActions_;
    extui::IMGUIManager imgui_;
    UpdaterAPIHelpers updaterApi_;
    std::atomic<uint32_t> nextGenerationId_{ 1 };

    ExtenderConfig config_;
    bool postStartupDone_{ false };
    bool updateHooksAdded_{ false };

    std::thread* luaDebuggerThread_{ nullptr };
    std::unique_ptr<LuaDebugInterface> luaDebugInterface_;
    std::unique_ptr<lua::dbg::DebugMessageHandler> luaDebugMsgHandler_;
    std::unique_ptr<lua::dbg::Debugger> luaDebugger_;

    GameVersionInfo gameVersion_;
    ULONGLONG startTime_{ 0 };

    void WarnIfOffline();
    void OnCoreLibInit(void * self);
    void OnAppUpdatePaths(App* self);
    void OnAppLoadGraphicSettings(App* self);
    void OnAppInitGameBegin(App* self);
    void OnAppInitGameEnd(App* self, bool result);
    void OnInitializeResourceManagerBegin(App* self);
    void OnInitializeResourceManagerEnd(App* self);
    void OnLoadPackFilesBegin(void* self, bool a1, bool a2);
    void OnLoadPackFilesEnd(void* self, bool a1, bool a2);
    void OnLoadFromContextFolderBegin(void* self, Path const* folder, bool fallback, bool a3);
    void OnLoadFromContextFolderEnd(void* self, Path const* folder, bool fallback, bool a3, bool result);
    void OnAdvanceHintBegin(void* self);
    void OnAdvanceHintEnd(void* self);
    void OnImageSequenceLoaded(void* self, void* a1, void* a2);
    void OnGameUIUpdate(void* self, void* time);
    void OnArgumentGetValue(void* self, char const* key, STDString* value, bool found);
    void HookStateMachineUpdates();
    void OnStatsLoad(stats::RPGStats__LoadProc* wrapped, stats::RPGStats* mgr, Array<STDString>* paths);
    void OnStatsLoadGuarded(stats::RPGStats__LoadProc* wrapped, stats::RPGStats* mgr, Array<STDString>* paths);
    void OnECSUpdate(ecs::EntityWorld__UpdateProc* wrapped, ecs::EntityWorld* entityWorld, GameTime const& time);
    void OnECSUpdateGuarded(ecs::EntityWorld__UpdateProc* wrapped, ecs::EntityWorld* entityWorld, GameTime const& time);
    void OnECSFlushECBs(ecs::EntityWorld* entityWorld);
    void OnFindPath(AiGrid* self, AiPathId pathId);
    FileReader * OnFileReaderCreate(FileReader__CtorProc* next, FileReader * self, Path const& path, unsigned int type, unsigned int unknown);
};

extern std::unique_ptr<ScriptExtender> gExtender;

}
