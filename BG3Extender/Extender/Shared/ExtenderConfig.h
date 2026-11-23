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
