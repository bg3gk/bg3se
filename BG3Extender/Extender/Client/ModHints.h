#pragma once

// The loading screen's hints, with the mods' own hint files appended.
//
// The game loads one hint file, Public/Game/Hints/Hints.lsx, into ecl::HintManager during
// App::InitializeResourceManager (whoever shadows that file last replaces the whole list). Here each
// module of the game's module list (Extender/Client/GameModules.h: the modules the game loads, in
// its load order) may ship a file of the same format in its own folder,
// Public/<Folder>/Hints/Hints.lsx. At the client's first StartLoading (the module list is known, the
// mods' files are reachable, and the loading screen has not drawn a hint yet) each such file is
// loaded through the game's own ls::FileFormatIO::LoadFile into a second ecl::HintManager, and its
// entries are composed into the game's list: an entry whose HintKey is new is appended, an entry
// whose HintKey is already in the list replaces that entry in place (a later mod wins; a warning
// names both).
//
// The hints follow the game's reload of its modules: a change of the mod list in the game's mod
// manager is applied by the game at the next session's start (the client's UnloadModule and
// LoadModule). At the end of every client LoadModule the modules the game loaded are compared with
// the ones the list was composed from; when they differ, the list is composed again - the game's
// own file loaded anew through the same loader, then every file of the new module list - so a
// disabled mod's hints go and an enabled one's come. That composition is put into the game's list
// on the interface's thread (the thread the loading screen draws its hints on), before the next
// hint is drawn; the list it replaces is destroyed by the game's own destructor.
//
// The entries are the game's own 0x38-byte ecl::Hint objects, moved as they are (their FixedString,
// TranslatedString and string keep their references); the ones that are replaced are destroyed by
// the game's own ecl::HintManager destructor.

#include <Extender/Client/GameModules.h>
#include <vector>

BEGIN_NS(ecl::mod_hints)

// Who gave a hint: the game, or a mod by its UUID, name and folder.
struct HintSource
{
    // "Game", or the mod's UUID.
    STDString Source;
    STDString Name;
    STDString Folder;
};

struct ComposedHint
{
    STDString HintKey;
    HintSource From;
    // The source whose entry of the same key this one replaced (Source empty when none).
    HintSource Replaced;
};

// One module's hint file, as it was looked for.
struct HintFile
{
    HintSource Mod;
    // Public/<Folder>/Hints/Hints.lsx
    STDString Path;
    bool Found{ false };
    // ls::FileFormatIO::LoadFile returned true.
    bool Loaded{ false };
    uint32_t Count{ 0 };
    uint32_t Added{ 0 };
    uint32_t Replaced{ 0 };
};

struct HintsRecord
{
    // The client's first StartLoading was met.
    bool Seen{ false };
    // The player's switch "ModHints" was on at that moment.
    bool Enabled{ true };
    // The game's list was rewritten (false when no mod gave a hint at the first start).
    bool Written{ false };
    // "StartLoading" (the first composition) or "LoadModule" (composed again after the game loaded
    // another module list).
    STDString Moment;
    // How many compositions were put into the game's list (1 after the start, +1 per reload).
    uint32_t Compositions{ 0 };
    // A composition made at a reload and not yet put into the game's list (the interface's thread
    // takes it on its next update).
    bool Pending{ false };
    // How many entries the game's own file gave, and how many the list holds after the composition.
    uint32_t GameCount{ 0 };
    uint32_t Count{ 0 };
    // The module list the composition read (GameModules.h).
    game_modules::ModuleList Modules;
    std::vector<HintFile> Files;
    // The composed list in order: the game's entries (some replaced in place), then the appended.
    std::vector<ComposedHint> Entries;
    std::vector<STDString> Warnings;
};

// The composition in the game's list now (a copy).
HintsRecord GetHintsRecord();

// Called from the client's StartLoading listener; composes once, at the first.
void OnStartLoading();

// Called at the end of every client LoadModule with the modules the game loaded: composes again when
// they differ from the ones the list was composed from.
void OnModuleLoaded(game_modules::ModuleList const& loaded);

// Called on the interface's thread (before its update, and before the loading screen draws a hint):
// puts a pending composition into the game's list.
void OnInterfaceThread();

END_NS()
