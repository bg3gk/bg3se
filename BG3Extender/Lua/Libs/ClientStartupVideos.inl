#include <Extender/Client/StartupVideos.h>
#include <Extender/Client/GameModules.h>
#include <Extender/Shared/StartupTimeline.h>
#include <Extender/Shared/LoadTrace.h>
#include <Extender/Shared/RenderCut.h>
#include <Extender/Shared/Headless.h>

/// <lua_module>StartupVideos</lua_module>
BEGIN_NS(ecl::lua::startup_videos)

using namespace bg3se::lua;
using namespace bg3se::ecl::startup_videos;

namespace
{
    void PushStrings(lua_State* L, std::vector<STDString> const& strings)
    {
        lua_createtable(L, (int)strings.size(), 0);
        int i = 1;
        for (auto const& s : strings) {
            push(L, s);
            lua_rawseti(L, -2, i++);
        }
    }

    // { Uuid, ModTable?, Name, Folder? } on top of the stack.
    void PushMod(lua_State* L, ModIdentity const& m)
    {
        lua_createtable(L, 0, 4);
        settable(L, "Uuid", m.Uuid);
        if (!m.ModTable.empty()) settable(L, "ModTable", m.ModTable);
        settable(L, "Name", m.Name);
        if (!m.Folder.empty()) settable(L, "Folder", m.Folder);
    }

    void PushComposition(lua_State* L, Composition const& c)
    {
        lua_createtable(L, (int)c.Entries.size(), 0);
        int i = 1;
        for (auto const& e : c.Entries) {
            lua_createtable(L, 0, 5);
            settable(L, "Id", e.Id);
            settable(L, "Video", e.Video);
            settable(L, "Source", e.Source);
            if (!e.SourceMod.Uuid.empty()) {
                PushMod(L, e.SourceMod);
                lua_setfield(L, -2, "Mod");
            }
            if (!e.ReplacedBy.Uuid.empty()) {
                PushMod(L, e.ReplacedBy);
                lua_setfield(L, -2, "ReplacedBy");
            }
            lua_rawseti(L, -2, i++);
        }
        lua_setfield(L, -2, "Entries");

        PushStrings(L, c.Warnings);
        lua_setfield(L, -2, "Warnings");
    }
}

// Ext.StartupVideos.GetPlaylist() -> { Entries = { { Id, Video, Source, Mod?, ReplacedBy? } },
// Warnings, Enabled, Written, Game, Mods = { { Uuid, ModTable?, Name, Folder, MetaReadable,
// ConfigReadable, DeclarationFound } }, ModuleList = { Source, Moment }, ... }:
// the composition the game's first StartLoading was given, and what it was made from. Mods are the
// modules of the game's module list, in its load order; ModuleList names that list
// (Ext.StartupVideos.GetModuleLists gives it whole).
UserReturn GetPlaylist(lua_State* L)
{
    auto const& rec = GetStartupRecord();
    lua_newtable(L);
    PushComposition(L, rec.Result);
    settable(L, "Seen", rec.Seen);
    settable(L, "Written", rec.Written);
    settable(L, "Enabled", rec.Enabled);
    PushStrings(L, rec.GameEntries);
    lua_setfield(L, -2, "Game");
    settable(L, "SettingsMods", rec.SettingsMods);
    settable(L, "AvailableMods", rec.AvailableMods);
    settable(L, "LoadOrderedModules", rec.LoadOrderedModules);
    lua_createtable(L, 0, 2);
    settable(L, "Source", rec.ModuleListSource);
    settable(L, "Moment", rec.ModuleListMoment);
    lua_setfield(L, -2, "ModuleList");

    lua_createtable(L, (int)rec.Mods.size(), 0);
    int i = 1;
    for (auto const& m : rec.Mods) {
        PushMod(L, m.Mod);
        settable(L, "MetaReadable", m.MetaReadable);
        settable(L, "ConfigReadable", m.ConfigReadable);
        settable(L, "DeclarationFound", m.DeclarationFound);
        lua_rawseti(L, -2, i++);
    }
    lua_setfield(L, -2, "Mods");
    return 1;
}

// Ext.StartupVideos.Compose(declarations) -> { Entries, Warnings }: the composer on the given
// declarations, in the order given, over the game's own entries. Each declaration is a table of
// the StartupVideos.json format with the mod's identity: { Uuid, ModTable?, Name?, Hide, Replace, Add }.
UserReturn Compose(lua_State* L)
{
    luaL_checktype(L, 1, LUA_TTABLE);

    std::vector<Declaration> decls;
    std::vector<STDString> warnings;
    auto count = (int)lua_rawlen(L, 1);
    for (int i = 1; i <= count; i++) {
        lua_rawgeti(L, 1, i);
        if (lua_type(L, -1) == LUA_TTABLE) {
            json::StringifyContext ctx;
            ctx.Beautify = false;
            auto text = json::Stringify(L, ctx, lua_absindex(L, -1));
            Declaration decl;
            if (ParseDeclaration(ModIdentity{}, text, decl, warnings)) {
                decls.push_back(std::move(decl));
            }
        } else {
            warnings.push_back("A declaration that is not a table is left out");
        }
        lua_pop(L, 1);
    }

    auto const& rec = GetStartupRecord();
    std::vector<STDString> game = rec.GameEntries;
    if (game.empty()) {
        // The fork did not see the startup (loaded late): the game's code constant.
        game.push_back("Splash_Logo_Larian");
        warnings.push_back("The game's startup playlist was not seen; its one entry Splash_Logo_Larian is assumed");
    }

    // A reference may name by ModTable any mod enabled at the start, as well as the ones given.
    std::vector<ModIdentity> mods;
    for (auto const& m : rec.Mods) mods.push_back(m.Mod);

    auto result = bg3se::ecl::startup_videos::Compose(game, decls, mods, true);
    result.Warnings.insert(result.Warnings.begin(), warnings.begin(), warnings.end());

    lua_newtable(L);
    PushComposition(L, result);
    return 1;
}

namespace
{
    // The fade phase of the client game state machine (ecl::GameStateMachine+0x28).
    char const* FadeName(int32_t v)
    {
        switch (v) {
        case 0: return "None";
        case 1: return "FadingOut";
        case 2: return "Black";
        case 3: return "FadingIn";
        default: return "Unknown";
        }
    }
}

// Ext.StartupVideos.Timeline() -> { { Ms, Side, What, ... }, ..., Dropped = n }: what the extender
// recorded since its first moment in the process, in order, with milliseconds since the process
// was created. Side is "Extender", "Client" or "Server". What, and the fields each kind adds:
// - ExtenderLoaded: the counters (below);
// - Step: Name (the game's function, or the extender's), Phase ("Begin", "End", or none for a
//   single moment), the counters: a step of the engine's start before the first game state;
// - FileOpened: Name (the end of the path) of a file the engine opened before the client's first
//   game state (the first of them only; FileOpens counts them all);
// - State: From, To (the state names of that side), the counters;
// - the counters, on ExtenderLoaded, Step and State: IoReadBytes, IoReadOps, CpuMs (the process's
//   bytes read, read operations, CPU time of all its threads so far), FileOpens (files opened
//   through ls::FileReader since the extender hooked it);
// - PlaylistWritten: Name (the entry and its count);
// - VideoStarted: Name (the video and how many still wait); VideoStopped;
// - LoadState: Name (the active load state's type as module+0xRVA of its vtable, or none);
// - LoadingScreenShown, LoadingScreenHidden;
// - Fade: From, To (None, FadingOut, Black, FadingIn) of the client game state machine;
// - UIState: Name, Parent, Depth, Change ("Entered" or "Left") of the interface's state machine;
// - LoadingProgress: Progress, ProgressTotal, Name ("LocalProgress/LocalProgressTotal") of the
//   loading screen manager (LocalProgress in tenths of its total);
// - HintShown: Name (the HintKey), Index, Count: the hint the loading screen drew;
// - HintsComposed: GameCount, Count, Name, Moment: the mods' hints composed into the game's list
//   (Moment "StartLoading": the first composition; "reload": composed again after the game loaded
//   another module list; "installed": that composition put in the game's list, on the
//   interface's thread);
// - ModuleList: Modules, SettingsCount, Name (where the list came from and whether it is the one
//   taken before), Moment ("StartLoading", "LoadModule"): the game's module list taken for the
//   early readers (Ext.StartupVideos.GetModuleLists);
// - TimelinePlayback: Name (the timeline resource), Type (TimelineType), From, To (PlaybackState,
//   or -1 when the timeline entity appeared / went away).
// - WallpaperSet: Index (1..), Count, Name (the image), Slot, FadeIn, FadeOut (milliseconds): one
//   row per image of a slideshow when its control is made ready, in the order it will show them;
// - WallpaperShown: Name (the image), Index (its position in the set, 0 for a single image),
//   Count (the set's size, 1 for a single image), Look ("sequence" or "single"): the wallpaper a
//   live element shows changed;
// - WallpaperGone: the same fields, of the image last shown: the wallpaper's element left the
//   interface (its widget went);
// - the three wallpaper rows carry Owner (the x:Name of the widget the element lies in: `root` for
//   the game's loading screen, a mod's page by its own name), OwnerData (the type of that widget's
//   DataContext) and Visible (the element's IsVisible);
// - EarlyModInterface: Merge ("early": the mods' interface merged at the first StartLoading;
//   "game": the game's own merge), Modules (the modules given to ui::ModManager::RegisterMods),
//   Registered (its answer; absent when not run), Faulted, Name (the modules' folders or why it was
//   not run).
// Every row carries Row, its number in the record (1..). Timeline(since) gives only the rows after
// row `since` (a cheap read for a caller that keeps where it stopped); the list's field Count is
// the number of rows recorded so far.
// Events past the record's capacity are dropped; the count is in the list's field Dropped.
UserReturn Timeline(lua_State* L, std::optional<int64_t> since)
{
    namespace tl = bg3se::startup_timeline;
    static char const* const sides[] = { "Extender", "Client", "Server" };
    static char const* const whats[] = { "ExtenderLoaded", "State", "PlaylistWritten", "VideoStarted",
        "VideoStopped", "LoadState", "LoadingScreenShown", "LoadingScreenHidden", "Fade", "UIState",
        "Step", "FileOpened", "LoadingProgress", "HintShown", "HintsComposed", "TimelinePlayback",
        "WallpaperSet", "WallpaperShown", "WallpaperGone", "EarlyModInterface", "ModuleList" };

    auto count = tl::Count();
    uint32_t first = 0;
    if (since && *since > 0) first = (*since >= (int64_t)count) ? count : (uint32_t)*since;
    lua_createtable(L, (int)(count - first), 2);
    for (uint32_t i = first; i < count; i++) {
        auto const& e = tl::Get(i);
        lua_createtable(L, 0, 9);
        settable(L, "Row", (int64_t)i + 1);
        settable(L, "Ms", e.Ms);
        settable(L, "Side", sides[(unsigned)e.EventSide]);
        settable(L, "What", whats[(unsigned)e.Kind]);
        switch (e.Kind) {
        case tl::What::State:
            if (e.EventSide == tl::Side::Server) {
                settable(L, "From", EnumInfo<esv::GameState>::Find((esv::GameState)e.From).GetString());
                settable(L, "To", EnumInfo<esv::GameState>::Find((esv::GameState)e.To).GetString());
            } else {
                settable(L, "From", EnumInfo<ecl::GameState>::Find((ecl::GameState)e.From).GetString());
                settable(L, "To", EnumInfo<ecl::GameState>::Find((ecl::GameState)e.To).GetString());
            }
            break;

        case tl::What::Fade:
            settable(L, "From", FadeName(e.From));
            settable(L, "To", FadeName(e.To));
            break;

        case tl::What::UIState:
            settable(L, "Change", e.To != 0 ? "Entered" : "Left");
            settable(L, "Depth", e.From);
            if (e.Parent[0] != 0) settable(L, "Parent", e.Parent);
            break;

        case tl::What::Step:
            if (e.From == tl::StepBegin) settable(L, "Phase", "Begin");
            if (e.From == tl::StepEnd) settable(L, "Phase", "End");
            break;

        case tl::What::LoadingProgress:
            settable(L, "Progress", e.From);
            settable(L, "ProgressTotal", e.To);
            break;

        case tl::What::HintShown:
            settable(L, "Index", e.From);
            settable(L, "Count", e.To);
            break;

        case tl::What::HintsComposed:
            settable(L, "GameCount", e.From);
            settable(L, "Count", e.To);
            if (e.Parent[0] != 0) settable(L, "Moment", e.Parent);
            break;

        case tl::What::ModuleList:
            settable(L, "Modules", e.From);
            settable(L, "SettingsCount", e.To);
            if (e.Parent[0] != 0) settable(L, "Moment", e.Parent);
            break;

        case tl::What::TimelinePlayback:
            settable(L, "From", e.From);
            settable(L, "To", e.To);
            if (e.Parent[0] != 0) settable(L, "Type", (int32_t)atoi(e.Parent));
            break;

        case tl::What::WallpaperSet:
        {
            settable(L, "Index", e.From);
            settable(L, "Count", e.To);
            long long slot{ 0 }, fadeIn{ 0 }, fadeOut{ 0 };
            if (sscanf_s(e.Parent, "%lld/%lld/%lld", &slot, &fadeIn, &fadeOut) == 3) {
                settable(L, "Slot", (int64_t)slot);
                settable(L, "FadeIn", (int64_t)fadeIn);
                settable(L, "FadeOut", (int64_t)fadeOut);
            }
            settable(L, "Owner", e.Owner);
            settable(L, "OwnerData", e.OwnerData);
            if (e.Visible >= 0) settable(L, "Visible", e.Visible != 0);
            break;
        }

        case tl::What::WallpaperShown:
        case tl::What::WallpaperGone:
            settable(L, "Index", e.From);
            settable(L, "Count", e.To);
            if (e.Parent[0] != 0) settable(L, "Look", e.Parent);
            settable(L, "Owner", e.Owner);
            settable(L, "OwnerData", e.OwnerData);
            if (e.Visible >= 0) settable(L, "Visible", e.Visible != 0);
            break;

        case tl::What::EarlyModInterface:
            settable(L, "Merge", e.Parent);
            settable(L, "Modules", e.From);
            if (e.To >= 0) settable(L, "Registered", e.To != 0);
            if (e.To == -2) settable(L, "Faulted", true);
            break;

        default:
            break;
        }
        if (e.HasCounters) {
            settable(L, "IoReadBytes", (int64_t)e.IoReadBytes);
            settable(L, "IoReadOps", (int64_t)e.IoReadOps);
            settable(L, "CpuMs", e.CpuMs);
            settable(L, "FileOpens", (int64_t)e.FileOpens);
        }
        if (e.Name[0] != 0) settable(L, "Name", e.Name);
        lua_rawseti(L, -2, (int)(i - first) + 1);
    }
    settable(L, "Count", (int64_t)count);
    settable(L, "Dropped", tl::Dropped());
    return 1;
}

// Ext.StartupVideos.GetModuleLists() -> { { Source, Moment, Ms, SettingsCount, Modules = { { Uuid,
// Name, Folder } }, Warnings }, ... }: every game's module list the early readers took
// (Extender/Client/GameModules.h), in order: the first StartLoading's (Source "LoadModFromSettings":
// the game's resolver called there; "Settings": the fallback, modsettings.lsx's own order), then
// one at the end of each client LoadModule (Source "LoadOrderedModules": what the game loaded).
// The startup videos, the mods' hints and the mods' interface on the first loading screen read the
// first; the hints are composed again from a later one when it differs.
UserReturn GetModuleLists(lua_State* L)
{
    auto lists = bg3se::ecl::game_modules::History();
    lua_createtable(L, (int)lists.size(), 0);
    int i = 1;
    for (auto const& list : lists) {
        lua_createtable(L, 0, 6);
        settable(L, "Source", list.Source);
        settable(L, "Moment", list.Moment);
        settable(L, "Ms", list.Ms);
        settable(L, "SettingsCount", list.SettingsCount);
        lua_createtable(L, (int)list.Modules.size(), 0);
        int j = 1;
        for (auto const& m : list.Modules) {
            lua_createtable(L, 0, 3);
            settable(L, "Uuid", m.Uuid);
            settable(L, "Name", m.Name);
            settable(L, "Folder", m.Folder);
            lua_rawseti(L, -2, j++);
        }
        lua_setfield(L, -2, "Modules");
        PushStrings(L, list.Warnings);
        lua_setfield(L, -2, "Warnings");
        lua_rawseti(L, -2, i++);
    }
    return 1;
}

// GateKit, measuring only (the switch LoadTrace): Ext.StartupVideos.LoadTraceDump(path, sinceMs?)
// writes the load's trace since sinceMs (process milliseconds, as the timeline's Ms; 0 or none =
// everything) into path and three .tsv files beside it; returns what was written or why not.
// Ext.StartupVideos.LoadTraceNow() gives the current time in that clock (a mark before a load);
// Ext.StartupVideos.LoadTraceReset() forgets everything recorded (call it while nothing loads).
STDString LoadTraceDump(STDString path, std::optional<double> sinceMs)
{
    return STDString(bg3se::load_trace::Dump(path.c_str(), sinceMs ? *sinceMs : 0.0));
}

double LoadTraceNow()
{
    return bg3se::startup_timeline::NowMs();
}

void LoadTraceReset()
{
    bg3se::load_trace::Reset();
}

// Ext.StartupVideos.LoadTraceServerTick(us?) -> the server worker's tick period in microseconds
// before the call (-1: not known); with `us` it is written (0: the server ticks without waiting).
// Measuring only (the switch LoadTrace).
int64_t LoadTraceServerTick(std::optional<int64_t> us)
{
    return bg3se::load_trace::ServerTickUs(us ? *us : -1);
}

// Ext.StartupVideos.LoadTracePrepareRunning(maxSeconds?) -> { Override, Min, Max, Waited, Quiet,
// FirstMin, FirstMax, Reads }: the client's PrepareRunning streaming poller as last read (Min/Max
// the least and most wait, Waited the time waited, Quiet the polls in a row with nothing
// streaming; First* as read at the state's first update). With maxSeconds the most wait of every
// later PrepareRunning is overridden (a negative number: the game's own again). Measuring only.
UserReturn LoadTracePrepareRunning(lua_State* L, std::optional<float> maxSeconds)
{
    auto before = bg3se::load_trace::PrepareRunningMaxWait(maxSeconds ? *maxSeconds : 0.0f, maxSeconds.has_value());
    auto p = bg3se::load_trace::LastPoller();
    lua_createtable(L, 0, 8);
    settable(L, "Override", (double)before);
    settable(L, "Min", (double)p.Min);
    settable(L, "Max", (double)p.Max);
    settable(L, "Waited", (double)p.Waited);
    settable(L, "Quiet", (int64_t)p.Quiet);
    settable(L, "FirstMin", (double)p.FirstMin);
    settable(L, "FirstMax", (double)p.FirstMax);
    settable(L, "Reads", (int64_t)p.Reads);
    return 1;
}

// GateKit (Extender/Shared/RenderCut.h, the switches RenderCut and FastLoadLuaGC):
// Ext.StartupVideos.RenderCut(mode?) -> { Installed, LuaGcInstalled, Mode, Frames, Skipped,
// ClientTicks, ServerTicks, GcHeld, GcReleased, ClientCpuMs, ServerCpuMs, MainCpuMs, ProcessCpuMs,
// Ms, PrivateMB, WorkingSetMB }: the counters now (two reads give rates); with `mode` the main
// thread's frames are drawn (0), skipped (1) or one in `mode` drawn (Mode is the one before).
UserReturn RenderCut(lua_State* L, std::optional<int> mode)
{
    auto before = bg3se::render_cut::SetMode(mode ? *mode : -1);
    auto c = bg3se::render_cut::Read();
    lua_createtable(L, 0, 16);
    settable(L, "Installed", c.Installed);
    settable(L, "LuaGcInstalled", c.LuaGcInstalled);
    settable(L, "Mode", (int64_t)before);
    settable(L, "Frames", (int64_t)c.Frames);
    settable(L, "Skipped", (int64_t)c.Skipped);
    settable(L, "RenderFrames", (int64_t)c.RenderFrames);
    settable(L, "RenderFramesOff", (int64_t)c.RenderFramesOff);
    settable(L, "ClientTicks", (int64_t)c.ClientTicks);
    settable(L, "ServerTicks", (int64_t)c.ServerTicks);
    settable(L, "GcHeld", (int64_t)c.GcHeld);
    settable(L, "GcReleased", (int64_t)c.GcReleased);
    settable(L, "ClientCpuMs", c.ClientCpuMs);
    settable(L, "ServerCpuMs", c.ServerCpuMs);
    settable(L, "MainCpuMs", c.MainCpuMs);
    settable(L, "ProcessCpuMs", c.ProcessCpuMs);
    settable(L, "Ms", c.Ms);
    settable(L, "PrivateMB", c.PrivateMB);
    settable(L, "WorkingSetMB", c.WorkingSetMB);
    settable(L, "SkipPaceUs", (int64_t)c.SkipPaceUs);       // GateKit W3 round 7: RenderCutSkipPaceUs
    settable(L, "PacedSleeps", (int64_t)c.PacedSleeps);
    settable(L, "PacedSleptMs", c.PacedSleptMs);
    return 1;
}

// GateKit (Extender/Shared/Headless.h, the switches IdleSleep and StubFileExtensions):
// Ext.StartupVideos.Headless() -> { IdleSleepInstalled, Waits, Slept, SleptMs, StubExtensions,
// StubbedOpens }: the counters now.
UserReturn Headless(lua_State* L)
{
    auto c = bg3se::headless::Read();
    lua_createtable(L, 0, 6);
    settable(L, "IdleSleepInstalled", c.IdleSleepInstalled);
    settable(L, "Waits", (int64_t)c.Waits);
    settable(L, "Slept", (int64_t)c.Slept);
    settable(L, "SleptMs", c.SleptMs);
    settable(L, "StubExtensions", (int64_t)c.StubExtensions);
    settable(L, "StubbedOpens", (int64_t)c.StubbedOpens);
    return 1;
}

void RegisterStartupVideosLib()
{
    DECLARE_MODULE(StartupVideos, Client)
    BEGIN_MODULE()
    MODULE_FUNCTION(GetPlaylist)
    MODULE_FUNCTION(Compose)
    MODULE_FUNCTION(Timeline)
    MODULE_FUNCTION(GetModuleLists)
    MODULE_FUNCTION(LoadTraceDump)
    MODULE_FUNCTION(LoadTraceNow)
    MODULE_FUNCTION(LoadTraceReset)
    MODULE_FUNCTION(LoadTraceServerTick)
    MODULE_FUNCTION(LoadTracePrepareRunning)
    MODULE_FUNCTION(RenderCut)
    MODULE_FUNCTION(Headless)
    END_MODULE()
}

END_NS()
