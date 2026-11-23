#pragma once

// The game's module list, one for every early reader of mods' declarations (the startup videos,
// the mods' hints, the mods' interface on the first loading screen).
//
// The player's choice is modsettings.lsx (ls::ModManager's Settings, read by ecl::EoCClient::Load);
// what the game loads is its resolution: ls::ModManager::LoadModFromSettings makes the settings into
// the active module and ls::ModManager::BuildLoadOrderedModules into the load-ordered modules - the
// main module, its dependencies and the game's own modules (MainUI, ModBrowser, ...), in the game's
// order. The client's LoadModule (ecl::ModuleLoadSystem) makes the same resolution from the same
// settings; at the first StartLoading, before it, LoadOrderedModules is still empty.
//
// So at the client's first StartLoading the game's own resolver is called once on the client's
// manager (the call the game itself makes when the player toggles a mod in its mod manager), and
// every early reader reads the load-ordered modules it leaves: the list the game will load, in its
// order. The client's LoadModule then resolves the same settings again, as always. When the
// resolver is not found (a patch moved it) or faults, the readers fall back to the settings' own
// order, without the dependencies and the game's own modules (Source "Settings").
//
// At the end of every client LoadModule the list the game loaded is taken again (Moment
// "LoadModule"): at the first start it is compared with the one resolved at StartLoading; after a
// change of the mod list (the game reloads its modules at the next session's start) it is the new
// list, and the hints are composed again from it (Extender/Client/ModHints.h). Each list taken is
// a ModuleList row of the start's timeline.

#include <mutex>
#include <vector>

BEGIN_NS(ecl::game_modules)

struct ModuleEntry
{
    // The module's UUID, lower case.
    STDString Uuid;
    STDString Name;
    // Its folder (ls::ModuleInfo Directory): Mods/<Folder>, Public/<Folder>.
    STDString Folder;
};

struct ModuleList
{
    // "LoadModFromSettings": the game's resolver called at the first StartLoading, its
    // LoadOrderedModules read; "LoadOrderedModules": the client's load-ordered modules as the game
    // left them (the end of a LoadModule); "Settings": the fallback, modsettings.lsx's own order.
    STDString Source;
    // "StartLoading" or "LoadModule".
    STDString Moment;
    // Milliseconds since the process was created (the timeline's clock).
    double Ms{ 0 };
    // How many mods the settings hold (modsettings.lsx's entries).
    uint32_t SettingsCount{ 0 };
    std::vector<ModuleEntry> Modules;
    std::vector<STDString> Warnings;

    bool Empty() const { return Modules.empty() && Source.empty(); }
    // The same modules in the same order.
    bool SameModules(ModuleList const& other) const;
};

// At the client's first StartLoading: the game's resolver is called once and the list taken. Later
// calls give the list taken then.
ModuleList const& OnStartLoading();

// At the end of every client LoadModule: the client's load-ordered modules, as the game loaded them.
ModuleList OnModuleLoaded();

// The list taken at the first StartLoading (empty before it).
ModuleList Resolved();

// Every list taken, in order (the first StartLoading's, then one per client LoadModule).
std::vector<ModuleList> History();

END_NS()
