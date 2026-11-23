#pragma once

#include <cstdint>
#include <string>

BEGIN_SE()

struct ProfilerThreshold
{
    inline constexpr ProfilerThreshold(uint32_t error)
        : Error(error)
    {}

    uint32_t Error{ 0 };
};

struct ExtenderConfig
{
#if defined(OSI_EXTENSION_BUILD)
    bool CreateConsole{ false };
    bool DefaultToClientConsole{ false };
    bool EnableDebugger{ false };
    bool EnableLuaDebugger{ false };
#else
    bool CreateConsole{ true };
    bool DefaultToClientConsole{ false };
    bool EnableDebugger{ true };
    bool EnableLuaDebugger{ true };
#endif

    bool ExtendStory{ false };
    bool LogFailedCompile{ true };
    bool EnableLogging{ false };
    bool LogCompile{ false };
    bool LogRuntime{ false };
    bool SendCrashReports{ true };
    bool ForceCrashReporting{ false };
    // Whether the crash reporter window is started after a dump is written. A tool that collects
    // the dump itself turns this off.
    bool LaunchCrashReporter{ true };
    bool EnableAchievements{ true };
    bool DisableLauncher{ false };
    bool DisableStoryPatching{ false };
    bool DisableStoryCompilation{ true };
    bool InsanityCheck{ false };
    bool LocalMessagePassing{ true };
    bool Optick{ false };
    bool MergeVirtualTextures{ false };

    // A copy started by a tool must not take the keyboard from whoever is using the machine.
    // With this on, the game's attempts to bring its own window to the front are refused and
    // recorded instead of passed on (Extender/Client/WindowFocus.h). Off by default: a game
    // somebody started to play should raise itself when it wants to.
    bool KeepWindowInBackground{ false };

    // The window is put where no screen reaches, the moment it is created - for a copy something
    // else is about to draw (a deck's tile). Off by default: a copy started to be looked at
    // directly should be on a screen.
    bool KeepWindowOffScreen{ false };

    // Holding Shift as the game starts opens the console whatever CreateConsole says. A tool that
    // starts copies while somebody types elsewhere turns this off: a capital letter typed at the
    // wrong moment put a console on their desktop. On by default, as upstream behaves.
    bool ShiftOpensConsole{ true };

    // The player's switch for the startup videos: off, the game's first StartLoading is given no
    // video at all, whatever the mods declare (Extender/Client/StartupVideos.inl).
    bool StartupVideos{ true };

    // The player's switch for the mods' loading hints: off, the game's hint list stays as its own
    // file gives it, whatever the mods ship (Extender/Client/ModHints.inl).
    bool ModHints{ true };

    // The player's switch for the mods' interface on the first loading screen: off, the game merges
    // the mods' interface at its own time, at the end of LoadModule, and the first loading screen
    // is the game's alone (Extender/Client/EarlyModUI.inl).
    bool EarlyModInterface{ true };

    // A save asked for on the command line (-loadSaveGame, -continueGame) loaded without the main
    // menu: the menu's states the game queued at its start (LoadMenu, StopLoading, Menu) are taken
    // out of the client's queue when the game's own Init has queued the save's load behind them.
    // A combination the game does not make by itself, so off by default
    // (Extender/Client/DirectSaveLoad.inl).
    bool DirectSaveLoad{ false };

    // The game reads its command line narrow, in the system's ANSI code page, while its own strings
    // (save folders, paths) are UTF-8: a value such as a Cyrillic save name never matches. On, a
    // value the game reads through ls::ArgumentString::GetValue is given in UTF-8, from the
    // process's wide command line, when that is all it changes (Extender/Client/DirectSaveLoad.inl).
    bool WideCommandLine{ true };

    // Developer mode: a file of the executable's function starts (one hexadecimal offset a line);
    // coverage counters go on as the extender starts and a record is saved beside the file at every
    // state the client enters up to the main menu (Lua/Libs/DebugCover.inl). Empty: off.
    std::string CoverageAtStart;

    // GateKit, measuring only: the load's trace (Extender/Shared/LoadTrace.h) - every file opened and
    // read, the process's counters at every game state, a sampling profiler of the game's threads
    // every LoadTraceSampleMs. Off by default (it costs memory and some time).
    bool LoadTrace{ false };
    uint32_t LoadTraceSampleMs{ 5 };

    // GateKit, for automated runs: loads without the game's waits (Extender/Shared/FastLoad.h),
    // each part on its own. Off by default: a player's game keeps its pace and its fades.
    bool FastLoadClientFrames{ false };
    bool FastLoadServerTick{ false };
    bool FastLoadStreamingWait{ false };
    bool FastLoadStreamingGate{ false };   // W3 round 5: only the least wait cut (FastLoad.h)
    bool FastLoadFades{ false };
    // The game's own Lua collector held while the server loads (Extender/Shared/RenderCut.h).
    bool FastLoadLuaGC{ false };

    // GateKit, for automated runs: the main thread's frame (ls::RenderThread::Update) wrapped so it
    // can be skipped (nothing drawn, nothing presented), from the start when RenderCutSkip, else
    // when Ext.StartupVideos.RenderCut(1) asks (Extender/Shared/RenderCut.h). Off by default.
    bool RenderCut{ false };
    bool RenderCutSkip{ false };
    // GateKit (W3 round 5): with RenderCut, nothing drawn from the client's first Running on (the
    // start is drawn): the interface's view models of the party are made while the start draws -
    // with nothing drawn from the process start the host's VMCharacter misses IsPlayableCharacter
    // and its player, and a dialogue never shows its line (RenderCut.h). Off by default.
    bool RenderCutFromRunning{ false };
    // GateKit (W3 round 7), with RenderCut: a skipped update (nothing drawn) is paced to at most one
    // in this many microseconds - the main thread sleeps (a high-resolution waitable timer) out the
    // rest instead of looping straight into the next update (RenderCut.h). 0: off (the default).
    uint32_t RenderCutSkipPaceUs{ 0 };
    // Limit RenderCutSkipPaceUs to startup. Pacing an in-session load changes how long the server
    // can run before the client is ready.
    bool RenderCutSkipPaceStartOnly{ false };

    // GateKit (W3 round 4), for a headless game: the game threads' tick waits (unplaced::TickWait)
    // sleep on a high-resolution timer instead of spinning, the last 1.5 ms left to the game's own
    // precise wait (Extender/Shared/Headless.h). Off by default.
    bool IdleSleep{ false };
    // GateKit (W3 round 6), for differential checks: a wrapper of the treasure generation, inert
    // until Lua pins a seed (Ext.Debug.PinTreasure; Extender/Shared/Headless.h). Off by default.
    bool TreasurePin{ false };
    // GateKit (W3 round 7), measuring only: VirtualAlloc calls of at least this many MB written into
    // the start's timeline with their callers (Extender/Shared/Headless.h). 0: off (the default).
    uint32_t LargeAllocTraceMB{ 0 };
    // Record the exit code, callers and recent timeline rows in LogDirectory on self-termination.
    bool ExitWitness{ false };
    // GateKit (W3 round 4), measuring a headless game: the engine's opens (ls::FileReader) of paths
    // ending in one of these extensions (comma-separated, ".dds,.wem") find no file, as for a
    // file that is not there; the game's own fallback for a missing resource takes its place
    // (Extender/Shared/Headless.h). Empty: off.
    std::string StubFileExtensions;
    // With StubFileExtensions: the path a stubbed open is given instead (a small valid file of the
    // class, such as a 64x64 engine texture), so the game gets a valid object rather than none.
    // Empty: a path that does not exist.
    std::string StubFileTarget;

#if defined(OSI_EXTENSION_BUILD)
#if defined(_DEBUG)
    bool DeveloperMode{ true };
#else
    bool DeveloperMode{ false };
#endif // defined(_DEBUG)
#else
    bool DeveloperMode{ true };
#endif // defined(OSI_EXTENSION_BUILD)

    bool ClearOnReset{ true };
    uint32_t DebuggerPort{ 9999 };
    uint32_t LuaDebuggerPort{ 9998 };
    uint32_t DebugFlags{ 0 };
    std::wstring LogDirectory;
    // Where crash dumps go, one set of files per crash named by process id and time
    // (<pid>-<yyyymmdd>-<hhmmss>.dmp, .txt with the faulting module+0xRVA). Empty: one fixed file in
    // the temporary folder, overwritten by the next crash, as upstream does. When set, crash
    // reporting is on even with SendCrashReports off.
    std::wstring CrashDumpDirectory;
    std::wstring LuaBuiltinResourceDirectory;
    std::string CustomProfile;

    // Lua profiler configuration
    bool EnablePerfMessages{ true };
    ProfilerThreshold ProfilerLoadThreshold{ 50000 };
    ProfilerThreshold ProfilerLoadCallbackThreshold{ 50000 };
    ProfilerThreshold ProfilerCallbackThreshold{ 5000 };
    ProfilerThreshold ProfilerClientCallbackThreshold{ 2000 };
};

inline bool ProfilerShouldReport(uint64_t took, ExtenderConfig const& config, ProfilerThreshold const& threshold)
{
    return config.EnablePerfMessages
        && took >= threshold.Error;
}

#define PERF_REPORT(type, took, ...) WARN(##__VA_ARGS__)
#define PERF_SHOULD_REPORT(type, took) ProfilerShouldReport((took), gExtender->GetConfig(), gExtender->GetConfig().Profiler##type##Threshold)

END_SE()
