#include <Extender/Client/ModHints.h>

/// <lua_module>Hints</lua_module>
BEGIN_NS(ecl::lua::hints)

using namespace bg3se::lua;
using namespace bg3se::ecl::mod_hints;

namespace
{
    // { Source, Name?, Folder? } on top of the stack.
    void PushSource(lua_State* L, HintSource const& s)
    {
        lua_createtable(L, 0, 3);
        settable(L, "Source", s.Source);
        if (!s.Name.empty()) settable(L, "Name", s.Name);
        if (!s.Folder.empty()) settable(L, "Folder", s.Folder);
    }
}

// Ext.Hints.GetComposition() -> { Seen, Enabled, Written, Moment, Compositions, Pending, GameCount,
// Count, ModuleList, Entries, Files, Warnings }: how the loading screen's hint list
// (ecl::HintManager) in force was composed from the game's Hints.lsx and the
// Public/<Folder>/Hints/Hints.lsx of the modules of the game's module list.
// - Moment: "StartLoading" (the first composition) or "LoadModule" (composed again when the game
//   loaded another module list); Compositions: how many were put into the game's list; Pending: a
//   reload's composition waits for the interface's next update;
// - ModuleList: the list the composition read, { Source ("LoadModFromSettings", "Settings",
//   "LoadOrderedModules"), Moment, Count };
// - Entries: the list in order, { HintKey, Source ("Game" or the mod's UUID), Mod = { Source,
//   Name, Folder }?, Replaced = { Source, Name?, Folder? }? (whose entry of that key it replaced) };
// - Files: one per module of that list, in its order, { Source, Name, Folder, Path, Found, Loaded,
//   Count, Added, Replaced };
// - Warnings: every replacement (naming both sources) and every problem.
UserReturn GetComposition(lua_State* L)
{
    auto const& rec = GetHintsRecord();
    lua_newtable(L);
    settable(L, "Seen", rec.Seen);
    settable(L, "Enabled", rec.Enabled);
    settable(L, "Written", rec.Written);
    if (!rec.Moment.empty()) settable(L, "Moment", rec.Moment);
    settable(L, "Compositions", rec.Compositions);
    settable(L, "Pending", rec.Pending);
    settable(L, "GameCount", rec.GameCount);
    settable(L, "Count", rec.Count);

    lua_createtable(L, 0, 3);
    settable(L, "Source", rec.Modules.Source);
    settable(L, "Moment", rec.Modules.Moment);
    settable(L, "Count", (uint32_t)rec.Modules.Modules.size());
    lua_setfield(L, -2, "ModuleList");

    lua_createtable(L, (int)rec.Entries.size(), 0);
    int i = 1;
    for (auto const& e : rec.Entries) {
        lua_createtable(L, 0, 4);
        settable(L, "HintKey", e.HintKey);
        settable(L, "Source", e.From.Source);
        if (!e.From.Folder.empty()) {
            PushSource(L, e.From);
            lua_setfield(L, -2, "Mod");
        }
        if (!e.Replaced.Source.empty()) {
            PushSource(L, e.Replaced);
            lua_setfield(L, -2, "Replaced");
        }
        lua_rawseti(L, -2, i++);
    }
    lua_setfield(L, -2, "Entries");

    lua_createtable(L, (int)rec.Files.size(), 0);
    i = 1;
    for (auto const& f : rec.Files) {
        PushSource(L, f.Mod);
        settable(L, "Path", f.Path);
        settable(L, "Found", f.Found);
        settable(L, "Loaded", f.Loaded);
        settable(L, "Count", f.Count);
        settable(L, "Added", f.Added);
        settable(L, "Replaced", f.Replaced);
        lua_rawseti(L, -2, i++);
    }
    lua_setfield(L, -2, "Files");

    lua_createtable(L, (int)rec.Warnings.size(), 0);
    i = 1;
    for (auto const& w : rec.Warnings) {
        push(L, w);
        lua_rawseti(L, -2, i++);
    }
    lua_setfield(L, -2, "Warnings");
    return 1;
}

void RegisterHintsLib()
{
    DECLARE_MODULE(Hints, Client)
    BEGIN_MODULE()
    MODULE_FUNCTION(GetComposition)
    END_MODULE()
}

END_NS()
