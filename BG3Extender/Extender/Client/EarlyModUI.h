#pragma once

// The mods' interface on the first loading screen.
//
// The game merges the enabled mods' interface (their GUI folders: resource dictionaries such as
// GUI/Resources/Resources.xaml, texture metadata, state machines) in gui::GameUI::UpdateFromUI, once
// gui::GameUI::OnModuleLoaded has raised its "mods' interface pending" byte at the end of
// LoadModule: ui::ModManager::RegisterMods with the client's LoadOrderedModules, the texture
// metadata of each mod's data, gui::GameUI::ResetModdedLibs on the application resources' merged
// dictionaries, ui::StateMachine::ApplyMods. The loading screen of the first start is made at
// StartLoading, before that, and keeps the game's look until the interface's states are entered
// again in LoadMenu.
//
// Here the same block runs earlier: at the client's first StartLoading the modules are taken from
// the game's module list (Extender/Client/GameModules.h): the client's LoadOrderedModules as the
// game's own resolver (ls::ModManager::LoadModFromSettings) leaves them there - the set the game
// gives RegisterMods at its own merge, the game's own interface modules and the dependencies
// included, in its order. (When the resolver is not available: the enabled mods of
// modsettings.lsx found in AvailableMods, the base module left out.) On the interface's next
// update, before its state machine is updated (which makes the loading screen's widget), the merge
// block is run under the GameUI lock with that set, calling the game's functions with the
// arguments its own block gives them. The game's own merge at the end of LoadModule then runs as
// always; its row says whether the set it merges is the early one.
//
// The player's switch is "EarlyModInterface" (ExtenderConfig.h). Each merge, ours and the game's,
// is an EarlyModInterface row of the start's timeline.

BEGIN_NS(ecl::early_mod_ui)

// Called from the client's StartLoading listener; takes the module set once, at the first.
void OnStartLoading();

// Called first in gui::GameUI::UpdateFromUI (`gameUI`), on the interface's thread.
void BeforeGameUIUpdate(void* gameUI);

END_NS()
