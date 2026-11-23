#include <Extender/Client/StartupVideos.h>
#include <Extender/Shared/StartupTimeline.h>

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
// ConfigReadable, DeclarationFound } }, ... }:
// the composition the game's first StartLoading was given, and what it was made from.
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
// - UIState: Name, Parent, Depth, Change ("Entered" or "Left") of the interface's state machine.
// Events past the record's capacity are dropped; the count is in the list's field Dropped.
UserReturn Timeline(lua_State* L)
{
    namespace tl = bg3se::startup_timeline;
    static char const* const sides[] = { "Extender", "Client", "Server" };
    static char const* const whats[] = { "ExtenderLoaded", "State", "PlaylistWritten", "VideoStarted",
        "VideoStopped", "LoadState", "LoadingScreenShown", "LoadingScreenHidden", "Fade", "UIState",
        "Step", "FileOpened" };

    auto count = tl::Count();
    lua_createtable(L, (int)count, 1);
    for (uint32_t i = 0; i < count; i++) {
        auto const& e = tl::Get(i);
        lua_createtable(L, 0, 8);
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
        lua_rawseti(L, -2, (int)i + 1);
    }
    settable(L, "Dropped", tl::Dropped());
    return 1;
}

void RegisterStartupVideosLib()
{
    DECLARE_MODULE(StartupVideos, Client)
    BEGIN_MODULE()
    MODULE_FUNCTION(GetPlaylist)
    MODULE_FUNCTION(Compose)
    MODULE_FUNCTION(Timeline)
    END_MODULE()
}

END_NS()
