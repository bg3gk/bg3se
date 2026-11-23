#pragma once

// A save asked for on the command line, loaded without the main menu (the switch DirectSaveLoad,
// off by default).
//
// What the game does by itself: ecl::EocClient::Load queues the client's first states - InitMenu,
// StartLoading, Init, LoadModule, LoadMenu, StopLoading, Menu (after a fade). The Init state's Do
// (ecl::GameStateInit::Do) finds the save -loadSaveGame names (or the newest, -continueGame) and
// calls ecl::SavegameManager::TryLoad, which ends in the game's own start of a save
// (Eoc::StartGame, action 3, through ecl::SavegameManager::LoadGame): StartLoading (after a fade),
// StartServer and Idle are queued - behind the menu's states, so the save's load starts only once
// the main menu's level is loaded and shown (LoadMenu, ~16 s).
//
// What this adds: when the client leaves Init and the game has queued a save's load behind the
// menu's states, those states (LoadMenu, StopLoading, Menu) are taken out of the client's queue and
// disposed of as ecl::GameStateMachine::ClearQueue disposes of a queued state. Nothing is loaded by
// the extender: the save's load is the game's own, as queued by the game; the client goes
// Init -> LoadModule -> StartLoading -> StartServer -> ... -> Running, the order a level start
// (-testLoadLevel) takes, with the module loaded first as the menu's path loads it.
//
// It acts once per process, only when the queue is exactly the game's own: the menu's three states
// and, behind them, a StartLoading followed by a StartServer. Anything else is left as it is and
// says why in the timeline (a Step row, Parent "DirectSaveLoad").
//
// The save's name in any alphabet (the switch WideCommandLine, on by default; separable from the
// above): the game splits its WinMain command line - narrow, in the system's ANSI code page - into
// ls::ArgumentString, and reads -loadSaveGame (and every other value) from it through
// ls::ArgumentString::GetValue, while the save folders it matches against are UTF-8. A Cyrillic
// folder name in code page 1251 never matches, and in code page 1252 it becomes question marks.
// The extender starts after the split, so the value is mended where the game reads it: after
// GetValue, the same argument is taken from the process's wide command line (split as the game
// splits, at the same place) and given in UTF-8 - only when its ANSI form is exactly what the game
// read and its UTF-8 differs, so an ASCII value is never touched.

#include <vector>

BEGIN_NS(ecl::direct_save_load)

// What happened at the client's first exit from Init (also in the extender's log and the timeline).
struct Record
{
    // The switch's value.
    bool Enabled{ false };
    // The first Init -> LoadModule was met.
    bool Seen{ false };
    // The menu's states were taken out.
    bool Applied{ false };
    // Why not, or what was done.
    STDString Outcome;
    // The client's queue at that moment, state names in order (before any change).
    std::vector<STDString> QueueBefore;
    std::vector<STDString> QueueAfter;
};

// Called from the client's state listener for every change, before the new state's Enter.
void OnGameStateChanged(GameState fromState, GameState toState);

Record const& GetRecord();

// The post-hook of ls::ArgumentString::GetValue (any thread): `value` given in UTF-8 when the wide
// command line holds the same argument in another form.
void OnArgumentValue(char const* key, STDString* value, bool found);

END_NS()
