// The game's module list for the early readers (GameModules.h).
#include <Extender/Client/GameModules.h>
#include <Extender/Shared/StartupTimeline.h>
#include <GameDefinitions/Module.h>

BEGIN_NS(ecl::game_modules)

namespace
{
    constexpr char const* FromResolver = "LoadModFromSettings";
    constexpr char const* FromLoaded = "LoadOrderedModules";
    constexpr char const* FromSettings = "Settings";
    constexpr char const* AtStartLoading = "StartLoading";
    constexpr char const* AtLoadModule = "LoadModule";

    std::mutex gLock;
    bool gStartSeen{ false };
    ModuleList gResolved;
    std::vector<ModuleList> gHistory;

    STDString Lower(STDString s)
    {
        for (auto& c : s) {
            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        }
        return s;
    }

    STDString UuidOf(FixedString const& text, Guid const& guid)
    {
        if (text) {
            auto sv = text.GetStringView();
            return Lower(STDString(sv.data(), sv.size()));
        }
        return Lower(guid.ToString());
    }

    void TakeLoadOrdered(ModManager const& manager, ModuleList& list)
    {
        for (auto const& mod : manager.LoadOrderedModules) {
            ModuleEntry e;
            e.Uuid = UuidOf(mod.Info.ModuleUUIDString, mod.Info.ModuleUUID);
            e.Name = mod.Info.Name;
            e.Folder = mod.Info.Directory;
            list.Modules.push_back(std::move(e));
        }
    }

    void TakeSettings(ModManager const& manager, ModuleList& list)
    {
        for (auto const& mod : manager.Settings.Mods) {
            if (mod.Folder.empty()) continue;
            ModuleEntry e;
            e.Uuid = UuidOf(mod.ModuleUUIDString, mod.ModuleUUID);
            e.Name = mod.Name;
            e.Folder = mod.Folder;
            list.Modules.push_back(std::move(e));
        }
    }

    // The game's resolver, guarded: false when its code faulted.
    bool CallResolver(ModManager* manager)
    {
        __try {
            GetStaticSymbols().ls__ModManager__LoadModFromSettings(manager);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    void Row(ModuleList const& list, char const* name)
    {
        startup_timeline::Record(startup_timeline::Side::Client, startup_timeline::What::ModuleList,
            (int32_t)list.Modules.size(), (int32_t)list.SettingsCount, name, list.Moment.c_str());
    }
}

bool ModuleList::SameModules(ModuleList const& other) const
{
    if (Modules.size() != other.Modules.size()) return false;
    for (std::size_t i = 0; i < Modules.size(); i++) {
        if (Modules[i].Uuid != other.Modules[i].Uuid) return false;
    }
    return true;
}

ModuleList const& OnStartLoading()
{
    std::lock_guard<std::mutex> guard(gLock);
    if (gStartSeen) return gResolved;
    gStartSeen = true;

    ModuleList list;
    list.Moment = AtStartLoading;
    list.Ms = startup_timeline::NowMs();

    auto manager = GetStaticSymbols().GetModManagerClient();
    if (manager == nullptr) {
        list.Source = FromSettings;
        list.Warnings.push_back("The client's mod manager is not available; no mod is read");
        Row(list, "no client mod manager");
        gResolved = list;
        gHistory.push_back(std::move(list));
        return gResolved;
    }
    list.SettingsCount = manager->Settings.Mods.size();

    bool resolved{ false };
    if (GetStaticSymbols().ls__ModManager__LoadModFromSettings == nullptr) {
        list.Warnings.push_back("The game's resolver (ls::ModManager::LoadModFromSettings) was not found; the settings' own order is used, without dependencies");
    } else if (!CallResolver(manager)) {
        list.Warnings.push_back("The game's resolver (ls::ModManager::LoadModFromSettings) faulted; the settings' own order is used, without dependencies");
    } else if (manager->LoadOrderedModules.size() == 0) {
        list.Warnings.push_back("The game's resolver left no load-ordered module; the settings' own order is used, without dependencies");
    } else {
        resolved = true;
    }

    if (resolved) {
        list.Source = FromResolver;
        TakeLoadOrdered(*manager, list);
    } else {
        list.Source = FromSettings;
        TakeSettings(*manager, list);
    }

    Row(list, list.Source.c_str());
    INFO("Game modules: %d modules (%s) from %d settings entries", (int)list.Modules.size(), list.Source.c_str(), (int)list.SettingsCount);
    for (auto const& w : list.Warnings) {
        WARN("Game modules: %s", w.c_str());
    }

    gResolved = list;
    gHistory.push_back(std::move(list));
    return gResolved;
}

ModuleList OnModuleLoaded()
{
    std::lock_guard<std::mutex> guard(gLock);
    ModuleList list;
    list.Source = FromLoaded;
    list.Moment = AtLoadModule;
    list.Ms = startup_timeline::NowMs();

    auto manager = GetStaticSymbols().GetModManagerClient();
    if (manager == nullptr) {
        list.Warnings.push_back("The client's mod manager is not available");
        Row(list, "no client mod manager");
        gHistory.push_back(list);
        return list;
    }
    list.SettingsCount = manager->Settings.Mods.size();
    TakeLoadOrdered(*manager, list);

    // Compared with the list taken last: the first start's resolution, or the previous load's.
    ModuleList const* before = gHistory.empty() ? nullptr : &gHistory.back();
    char name[64];
    if (before == nullptr) {
        snprintf(name, sizeof(name), "%s", FromLoaded);
    } else {
        snprintf(name, sizeof(name), "%s: %s the list at %s", FromLoaded,
            list.SameModules(*before) ? "same as" : "differs from", before->Moment.c_str());
    }
    Row(list, name);
    gHistory.push_back(list);
    return list;
}

ModuleList Resolved()
{
    std::lock_guard<std::mutex> guard(gLock);
    return gResolved;
}

std::vector<ModuleList> History()
{
    std::lock_guard<std::mutex> guard(gLock);
    return gHistory;
}

END_NS()
