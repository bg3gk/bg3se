#include <Extender/Client/GameModules.h>
#include <GameDefinitions/Module.h>

// Ext.Mod on the client: the shared functions (Lua/Libs/Mod.inl) and the game's own mod list -
// what the player enabled, what the game loads, what waits for the next session's start - and the
// game's own enable and disable of a mod, the calls of its mod manager page. On the client this
// module is instantiated after the shared one and takes Ext.Mod's place, so it lists the shared
// functions too.
//
// The player's choice is the client's mod settings (ls::ModManager's Settings, modsettings.lsx).
// The game's mod manager page (gui::DCModBrowser::ToggleModEnabled) enables a mod with
// ecl::ModManagerClient::AddMod - the mod, then each of its dependencies that is a user mod (UGC)
// and not enabled - and
// disables one with ecl::ModManagerClient::RemoveMod. Both change the settings and then
// ecl::ModManagerClient::SaveSettingsAndReload: the settings' order resolved by dependencies, the
// file saved, the load-ordered modules resolved again (ls::ModManager::LoadModFromSettings), which
// clears the manager's byte +0x150. The modules themselves are loaded again only at the next
// session's start: a session started while that byte is 0 goes through the client's UnloadModule and
// LoadModule (Extender/Client/GameModules.h takes the list at the end of each).

/// <lua_module>Mod</lua_module>
BEGIN_NS(ecl::lua::mod)

using namespace bg3se::lua;

namespace
{
    // The manager's set of the user mods (UGC; ls::ModManager::IsUGCMod reads it): a HashSet<Guid>.
    // The mods installed in the profile hold it, the game's own modules do not.
    constexpr std::size_t UgcModsOffset = 0x178;

    STDString Lower(std::string_view s)
    {
        STDString out(s.data(), s.size());
        for (auto& c : out) {
            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        }
        return out;
    }

    STDString UuidOf(FixedString const& text, Guid const& guid)
    {
        if (text) return Lower(text.GetStringView());
        return Lower(guid.ToString());
    }

    STDString UuidOf(ModuleShortDesc const& d) { return UuidOf(d.ModuleUUIDString, d.ModuleUUID); }
    STDString UuidOf(Module const& m) { return UuidOf(m.Info.ModuleUUIDString, m.Info.ModuleUUID); }

    STDString VersionText(Version const& v)
    {
        char buf[64];
        snprintf(buf, sizeof(buf), "%llu.%llu.%llu.%llu", (unsigned long long)v.Major(), (unsigned long long)v.Minor(),
            (unsigned long long)v.Revision(), (unsigned long long)v.Build());
        return buf;
    }

    ModManager* Manager()
    {
        return GetStaticSymbols().GetModManagerClient();
    }

    int SettingsIndex(ModManager const& m, STDString const& uuid)
    {
        for (uint32_t i = 0; i < m.Settings.Mods.size(); i++) {
            if (UuidOf(m.Settings.Mods[i]) == uuid) return (int)i;
        }
        return -1;
    }

    int LoadIndex(ModManager const& m, STDString const& uuid)
    {
        for (uint32_t i = 0; i < m.LoadOrderedModules.size(); i++) {
            if (UuidOf(m.LoadOrderedModules[i]) == uuid) return (int)i;
        }
        return -1;
    }

    Module const* Available(ModManager const& m, STDString const& uuid)
    {
        for (auto const& mod : m.AvailableMods) {
            if (UuidOf(mod) == uuid) return &mod;
        }
        return nullptr;
    }

    bool IsUgc(ModManager const& m, Guid const& guid)
    {
        auto const& set = *reinterpret_cast<HashSet<Guid> const*>(reinterpret_cast<char const*>(&m) + UgcModsOffset);
        for (auto const& g : set.keys()) {
            if (g == guid) return true;
        }
        return false;
    }

    // ls::Module::GetShortDesc: the fields the settings keep of a module.
    ModuleShortDesc ShortDesc(Module const& mod)
    {
        ModuleShortDesc d{};
        d.ModuleUUIDString = mod.Info.ModuleUUIDString;
        d.ModuleUUID = mod.Info.ModuleUUID;
        d.Name = mod.Info.Name;
        d.ModVersion = mod.Info.ModVersion;
        d.PublishVersion = mod.Info.PublishVersion;
        d.Hash = mod.Info.Hash;
        d.Folder = mod.Info.Directory;
        d.PublishHandle = mod.Info.PublishHandle;
        return d;
    }

    // The game's calls, guarded: false when its code faulted.
    bool CallAddMod(ModManager* m, ModuleShortDesc const* d)
    {
        __try {
            GetStaticSymbols().ecl__ModManagerClient__AddMod(m, d);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    bool CallRemoveMod(ModManager* m, FixedString const* uuid)
    {
        __try {
            GetStaticSymbols().ecl__ModManagerClient__RemoveMod(m, uuid);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    // Why a change is refused now, or an empty string. The game's mod manager page is reached from
    // the main menu only; in a session the modules are loaded and the save checks its own mods.
    STDString Refusal(ModManager const* m)
    {
        if (m == nullptr) return "the client's mod manager is not available";
        if (GetStaticSymbols().ecl__ModManagerClient__AddMod == nullptr
            || GetStaticSymbols().ecl__ModManagerClient__RemoveMod == nullptr) {
            return "the game's ecl::ModManagerClient::AddMod or RemoveMod was not found in this build";
        }
        auto state = GetStaticSymbols().GetClientState();
        if (!state || *state != GameState::Menu) {
            STDString name = state ? EnumInfo<GameState>::Find(*state).GetString() : "none";
            return "the game changes its mod list in the main menu only (its mod manager page); the client is in " + name;
        }
        return {};
    }

    void PushUuids(lua_State* L, std::vector<STDString> const& uuids)
    {
        lua_createtable(L, (int)uuids.size(), 0);
        int i = 1;
        for (auto const& u : uuids) {
            push(L, u);
            lua_rawseti(L, -2, i++);
        }
    }

    std::vector<STDString> SettingsUuids(ModManager const& m)
    {
        std::vector<STDString> out;
        for (auto const& d : m.Settings.Mods) out.push_back(UuidOf(d));
        return out;
    }

    std::vector<STDString> LoadedUuids(ModManager const& m)
    {
        std::vector<STDString> out;
        for (auto const& mod : m.LoadOrderedModules) out.push_back(UuidOf(mod));
        return out;
    }

    // The list the client loaded last: the end of its last LoadModule, else the first StartLoading's.
    game_modules::ModuleList LastLoaded()
    {
        auto history = game_modules::History();
        for (auto it = history.rbegin(); it != history.rend(); ++it) {
            if (it->Moment == "LoadModule") return *it;
        }
        return history.empty() ? game_modules::ModuleList{} : history.front();
    }

    // { Pending, Byte, Since, Added, Removed, Reordered } on top of the stack.
    void PushPending(lua_State* L, ModManager const& m)
    {
        auto last = LastLoaded();
        std::vector<STDString> before;
        for (auto const& e : last.Modules) before.push_back(e.Uuid);
        auto now = LoadedUuids(m);

        std::vector<STDString> added, removed, keptNow, keptBefore;
        for (auto const& u : now) {
            if (std::find(before.begin(), before.end(), u) == before.end()) added.push_back(u);
            else keptNow.push_back(u);
        }
        for (auto const& u : before) {
            if (std::find(now.begin(), now.end(), u) == now.end()) removed.push_back(u);
            else keptBefore.push_back(u);
        }

        lua_createtable(L, 0, 6);
        settable(L, "Pending", m.Flag == 0);
        settable(L, "Byte", (uint32_t)m.Flag);
        if (!last.Moment.empty()) settable(L, "Since", last.Moment);
        PushUuids(L, added);
        lua_setfield(L, -2, "Added");
        PushUuids(L, removed);
        lua_setfield(L, -2, "Removed");
        settable(L, "Reordered", keptNow != keptBefore);
    }

    // What the game holds after a change: { Done, Reason?, Added, Removed, Settings, Loaded,
    // Pending } on top of the stack.
    void PushResult(lua_State* L, ModManager const* m, bool done, STDString const& reason,
        std::vector<STDString> const& added, std::vector<STDString> const& removed)
    {
        lua_createtable(L, 0, 7);
        settable(L, "Done", done);
        if (!reason.empty()) settable(L, "Reason", reason);
        PushUuids(L, added);
        lua_setfield(L, -2, "Added");
        PushUuids(L, removed);
        lua_setfield(L, -2, "Removed");
        if (m != nullptr) {
            PushUuids(L, SettingsUuids(*m));
            lua_setfield(L, -2, "Settings");
            PushUuids(L, LoadedUuids(*m));
            lua_setfield(L, -2, "Loaded");
            PushPending(L, *m);
            lua_setfield(L, -2, "Pending");
        }
    }

    // gui::DCModBrowser::ToggleModEnabled's enable: the mod, then each of its user-mod dependencies
    // not enabled. The UUIDs the settings gained are appended to `added`.
    STDString Enable(ModManager* m, STDString const& uuid, std::vector<STDString>& added)
    {
        auto mod = Available(*m, uuid);
        if (mod == nullptr) return "the mod is not among the game's available mods";
        auto desc = ShortDesc(*mod);
        if (!CallAddMod(m, &desc)) return "the game's ecl::ModManagerClient::AddMod faulted";
        added.push_back(uuid);
        // The module is found again: the call may have moved the available mods.
        mod = Available(*m, uuid);
        if (mod == nullptr) return {};
        std::vector<ModuleShortDesc> deps(mod->Dependencies.begin(), mod->Dependencies.end());
        for (auto const& dep : deps) {
            auto depUuid = UuidOf(dep);
            if (!IsUgc(*m, dep.ModuleUUID) || SettingsIndex(*m, depUuid) >= 0) continue;
            if (!CallAddMod(m, &dep)) return "the game's ecl::ModManagerClient::AddMod faulted on a dependency";
            added.push_back(depUuid);
        }
        return {};
    }

    STDString Disable(ModManager* m, STDString const& uuid, std::vector<STDString>& removed)
    {
        auto index = SettingsIndex(*m, uuid);
        if (index < 0) return "the mod is not enabled";
        // A copy: the call compacts the array the entry is in.
        FixedString key = m->Settings.Mods[index].ModuleUUIDString;
        if (!CallRemoveMod(m, &key)) return "the game's ecl::ModManagerClient::RemoveMod faulted";
        removed.push_back(uuid);
        return {};
    }
}

// Ext.Mod.GetList() -> { { UUID, Name, Folder, Version, Enabled, Loaded, Index?, LoadIndex?, Source,
// Main?, UGC, DependsOn, RequiredBy }, ... }: the client's mod settings in their order (Source
// "Settings", Index; the first is the adventure, Main), then the modules the game loads although
// the settings do not hold them (Source "Dependency" when a loaded module depends on it, "Game"
// otherwise), then the available mods neither enabled nor loaded (Source "Available"). Loaded and
// LoadIndex: the load-ordered modules the game resolved from the settings last; after a change in
// the main menu they are what the next session's start loads (Ext.Mod.Pending()).
UserReturn GetList(lua_State* L)
{
    auto m = Manager();
    lua_newtable(L);
    if (m == nullptr) return 1;

    struct Row { STDString Uuid; STDString Source; Module const* Mod; ModuleShortDesc const* Desc; };
    std::vector<Row> rows;
    std::vector<STDString> seen;
    for (auto const& d : m->Settings.Mods) {
        auto uuid = UuidOf(d);
        rows.push_back({ uuid, "Settings", Available(*m, uuid), &d });
        seen.push_back(uuid);
    }
    for (auto const& mod : m->LoadOrderedModules) {
        auto uuid = UuidOf(mod);
        if (std::find(seen.begin(), seen.end(), uuid) != seen.end()) continue;
        bool dependency{ false };
        for (auto const& other : m->LoadOrderedModules) {
            for (auto const& dep : other.Dependencies) {
                if (UuidOf(dep) == uuid) dependency = true;
            }
        }
        rows.push_back({ uuid, dependency ? "Dependency" : "Game", &mod, nullptr });
        seen.push_back(uuid);
    }
    for (auto const& mod : m->AvailableMods) {
        auto uuid = UuidOf(mod);
        if (std::find(seen.begin(), seen.end(), uuid) != seen.end()) continue;
        rows.push_back({ uuid, "Available", &mod, nullptr });
        seen.push_back(uuid);
    }

    int i = 1;
    for (auto const& r : rows) {
        auto index = SettingsIndex(*m, r.Uuid);
        auto loadIndex = LoadIndex(*m, r.Uuid);
        lua_createtable(L, 0, 13);
        settable(L, "UUID", r.Uuid);
        if (r.Desc != nullptr) {
            settable(L, "Name", r.Desc->Name);
            settable(L, "Folder", r.Desc->Folder);
            settable(L, "Version", VersionText(r.Desc->ModVersion));
        } else if (r.Mod != nullptr) {
            settable(L, "Name", r.Mod->Info.Name);
            settable(L, "Folder", r.Mod->Info.Directory);
            settable(L, "Version", VersionText(r.Mod->Info.ModVersion));
        }
        settable(L, "Enabled", index >= 0);
        settable(L, "Loaded", loadIndex >= 0);
        if (index >= 0) settable(L, "Index", index + 1);
        if (loadIndex >= 0) settable(L, "LoadIndex", loadIndex + 1);
        settable(L, "Source", r.Source);
        if (index == 0) settable(L, "Main", true);
        Guid guid = r.Desc != nullptr ? r.Desc->ModuleUUID : r.Mod->Info.ModuleUUID;
        settable(L, "UGC", IsUgc(*m, guid));

        std::vector<STDString> dependsOn, requiredBy;
        auto source = r.Mod != nullptr ? r.Mod : Available(*m, r.Uuid);
        if (source != nullptr) {
            for (auto const& dep : source->Dependencies) dependsOn.push_back(UuidOf(dep));
        }
        for (auto const& other : m->LoadOrderedModules) {
            for (auto const& dep : other.Dependencies) {
                if (UuidOf(dep) == r.Uuid) requiredBy.push_back(UuidOf(other));
            }
        }
        PushUuids(L, dependsOn);
        lua_setfield(L, -2, "DependsOn");
        PushUuids(L, requiredBy);
        lua_setfield(L, -2, "RequiredBy");
        lua_rawseti(L, -2, i++);
    }
    return 1;
}

// Ext.Mod.Pending() -> { Pending, Byte, Since, Added, Removed, Reordered }: whether the game loads
// its modules again at the next session's start - the manager's byte +0x150, 0 after the list was
// resolved anew (a change in the mod manager), back to 1 once a client LoadModule has loaded it - and
// how the load-ordered modules in force differ from the list the client loaded last (Since: the
// moment that list was taken, "LoadModule" or "StartLoading").
UserReturn Pending(lua_State* L)
{
    auto m = Manager();
    if (m == nullptr) {
        lua_newtable(L);
        return 1;
    }
    PushPending(L, *m);
    return 1;
}

// Ext.Mod.SetEnabled(uuid, on) -> { Done, Reason?, Added, Removed, Settings, Loaded, Pending }: the
// game's mod manager's toggle. On: the available mod through ecl::ModManagerClient::AddMod, then each
// of its user-mod (UGC) dependencies not enabled; off: ecl::ModManagerClient::RemoveMod (the mods that
// depend on it stay enabled; the game's resolver still loads it for them). Each call saves
// modsettings.lsx and resolves the list again; the modules are loaded at the next session's start.
// Client, main menu only (Reason says why otherwise).
UserReturn SetEnabled(lua_State* L, FixedString uuid, bool on)
{
    auto m = Manager();
    auto reason = Refusal(m);
    std::vector<STDString> added, removed;
    auto key = Lower(uuid.GetStringView());
    if (reason.empty()) {
        if (on) {
            reason = SettingsIndex(*m, key) >= 0 ? "the mod is already enabled" : Enable(m, key, added);
        } else if (SettingsIndex(*m, key) == 0) {
            reason = "the first entry of the settings is the adventure (the main module), not a mod the game's manager disables";
        } else {
            reason = Disable(m, key, removed);
        }
    }
    PushResult(L, m, reason.empty(), reason, added, removed);
    return 1;
}

// Ext.Mod.SetOrder({ uuid, ... }) -> { Done, Reason?, Added, Removed, Settings, Loaded, Pending, Same }:
// the given enabled mods put after the others, in the given order, by the game's own moves - each
// disabled (ecl::ModManagerClient::RemoveMod) and enabled again (ecl::ModManagerClient::AddMod),
// which appends it; the game's mod manager has no other order lever. Each save sorts the settings
// by dependencies (ls::ModuleSettings::ResolveAddonsOrder): Same says whether the given order held.
// Client, main menu only.
UserReturn SetOrder(lua_State* L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    std::vector<STDString> asked;
    auto n = lua_rawlen(L, 1);
    for (lua_Integer i = 1; i <= (lua_Integer)n; i++) {
        lua_rawgeti(L, 1, i);
        size_t len{ 0 };
        auto s = lua_tolstring(L, -1, &len);
        if (s != nullptr) asked.push_back(Lower(std::string_view(s, len)));
        lua_pop(L, 1);
    }

    auto m = Manager();
    auto reason = Refusal(m);
    if (reason.empty()) {
        for (std::size_t i = 0; i < asked.size() && reason.empty(); i++) {
            auto index = SettingsIndex(*m, asked[i]);
            if (index < 0) reason = "not enabled: " + asked[i];
            else if (index == 0) reason = "the first entry of the settings is the adventure (the main module): " + asked[i];
            else if (std::find(asked.begin(), asked.begin() + i, asked[i]) != asked.begin() + i) reason = "given twice: " + asked[i];
        }
    }

    std::vector<STDString> added, removed;
    for (std::size_t i = 0; i < asked.size() && reason.empty(); i++) {
        auto index = SettingsIndex(*m, asked[i]);
        ModuleShortDesc desc = m->Settings.Mods[index];
        auto mod = Available(*m, asked[i]);
        if (mod != nullptr) desc = ShortDesc(*mod);
        reason = Disable(m, asked[i], removed);
        if (reason.empty()) {
            if (CallAddMod(m, &desc)) added.push_back(asked[i]);
            else reason = "the game's ecl::ModManagerClient::AddMod faulted";
        }
    }

    PushResult(L, m, reason.empty(), reason, added, removed);
    if (m != nullptr) {
        std::vector<STDString> order;
        for (auto const& u : SettingsUuids(*m)) {
            if (std::find(asked.begin(), asked.end(), u) != asked.end()) order.push_back(u);
        }
        settable(L, "Same", order == asked);
    }
    return 1;
}

void RegisterClientModLib()
{
    DECLARE_MODULE(Mod, Client)
    BEGIN_MODULE()
    MODULE_NAMED_FUNCTION("IsModLoaded", bg3se::lua::mod::IsModLoaded)
    MODULE_NAMED_FUNCTION("GetLoadOrder", bg3se::lua::mod::GetLoadOrder)
    MODULE_NAMED_FUNCTION("GetMod", bg3se::lua::mod::GetMod)
    MODULE_NAMED_FUNCTION("GetBaseMod", bg3se::lua::mod::GetBaseMod)
    MODULE_NAMED_FUNCTION("GetModManager", bg3se::lua::mod::GetModManager)
    MODULE_FUNCTION(GetList)
    MODULE_FUNCTION(Pending)
    MODULE_FUNCTION(SetEnabled)
    MODULE_FUNCTION(SetOrder)
    END_MODULE()
}

END_NS()
