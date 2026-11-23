#pragma once

// The startup videos: what the game plays in its first StartLoading state, composed from the
// game's own playlist and the declarations of the enabled mods
// (Mods/<Folder>/ScriptExtender/StartupVideos.json), then written into the game's playlist.
//
// The composer is a pure function of the declarations and the game's entries; the rest of this
// file is the moment it is applied (the client's StartLoading listener, which runs before
// ecl::GameStateStartLoading::Enter reads the playlist) and what Lua reads afterwards.
//
// Ids (all their handling is in the "Ids" part of StartupVideos.inl):
// - a game entry is the game's own name, with no colon (Splash_Logo_Larian);
// - a mod's entry is <UUID>:<Id>, the UUID stamped from where the declaration was found and Id
//   the bare label its Add gives;
// - a reference (Before, After, Hide, Replace) to a mod's entry names the owner by UUID
//   (8-4-4-4-12 hex) or by ModTable (ScriptExtender/Config.json), then the label or "*" for all
//   of that mod's entries: <owner>:<Id>, <owner>:*. A bare name always means a game entry.

#include <vector>

BEGIN_NS(ecl::startup_videos)

// Who a mod is: the UUID of its meta.lsx, the ModTable of its ScriptExtender/Config.json (empty
// when it has none), its name and the folder its files live in.
struct ModIdentity
{
    STDString Uuid;
    STDString ModTable;
    STDString Name;
    STDString Folder;
};

struct VideoRef
{
    STDString Id;
    STDString Video;
};

struct AddEntry
{
    STDString Id;
    STDString Video;
    STDString Before;
    STDString After;
};

// One mod's declaration, in the order the file gives it.
struct Declaration
{
    ModIdentity Mod;
    std::vector<STDString> Hide;
    std::vector<VideoRef> Replace;
    std::vector<AddEntry> Add;
};

struct ComposedEntry
{
    // The game's own name for a game entry; <UUID>:<Id> for a mod's.
    STDString Id;
    // The name the game's player looks up as Video/<name>.bk2.
    STDString Video;
    // "Game", or the mod that added the entry.
    STDString Source;
    ModIdentity SourceMod;
    // The mod whose Replace gave the entry its video (Uuid empty when none).
    ModIdentity ReplacedBy;
};

struct Composition
{
    std::vector<ComposedEntry> Entries;
    std::vector<STDString> Warnings;
};

// Reads one declaration from JSON text. `mod` is stamped on it; a field of it left empty is taken
// from the text's own Uuid, ModTable, Name and Folder (declarations handed in from Lua). Problems
// become warnings.
bool ParseDeclaration(ModIdentity const& mod, StringView json, Declaration& out, std::vector<STDString>& warnings);

// The composition: the game's entries in the game's order, then the mods' Add entries in load
// order and file order, placed by their Before/After constraints (a stable topological sort, the
// base order as the tie-break), then Hide and Replace applied mod by mod in load order. `mods`
// are the enabled mods a reference may name by ModTable (with or without a declaration); with
// `enabled` false every entry is hidden.
Composition Compose(std::vector<STDString> const& gameEntries, std::vector<Declaration> const& declarations,
    std::vector<ModIdentity> const& mods, bool enabled);

// "Name (UUID, ModTable X)": how warnings and listings name a mod.
STDString DescribeMod(ModIdentity const& mod);

struct ModSeen
{
    ModIdentity Mod;
    // Mods/<Folder>/meta.lsx could be read at that moment (the mod's files are reachable).
    bool MetaReadable{ false };
    // Mods/<Folder>/ScriptExtender/Config.json could be read at that moment.
    bool ConfigReadable{ false };
    // Mods/<Folder>/ScriptExtender/StartupVideos.json was found.
    bool DeclarationFound{ false };
};

struct StartupRecord
{
    // The listener met the startup playlist (one entry, Splash_Logo_Larian).
    bool Seen{ false };
    // The composed list was written into the game's playlist.
    bool Written{ false };
    // The player's switch (StartupVideos in ScriptExtenderSettings.json).
    bool Enabled{ true };
    // The game's playlist as the game made it.
    std::vector<STDString> GameEntries;
    uint32_t SettingsMods{ 0 };
    uint32_t AvailableMods{ 0 };
    uint32_t LoadOrderedModules{ 0 };
    std::vector<ModSeen> Mods;
    std::vector<Declaration> Declarations;
    Composition Result;
};

StartupRecord const& GetStartupRecord();

// Called from the client's StartLoading listener with the state the machine has just made current.
void OnStartLoading(void* state);

END_NS()
