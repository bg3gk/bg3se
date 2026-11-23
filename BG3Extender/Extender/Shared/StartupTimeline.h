#pragma once

// The start's timeline: what happens from the extender's first moment in the process until Lua
// exists and after, with the time of each in milliseconds since the process was created. No Lua
// runs before LoadModule, so the order of the early start (the engine's steps and the files they
// open, StartLoading, the startup videos, the loading screen, the client's and the server's
// states, the screen fades, the interface's states) can only be read afterwards from here.
//
// The record is a fixed array: recording takes a slot with one atomic increment and copies into
// it, with no allocation; when it is full, later events are dropped (Dropped counts them).

#include <atomic>
#include <cstddef>
#include <cstdint>

BEGIN_NS(startup_timeline)

enum class Side : uint8_t
{
    Extender,
    Client,
    Server
};

enum class What : uint8_t
{
    // The extender's Initialize (its first moment with the game's symbols).
    ExtenderLoaded,
    // A game state change, From -> To (the state machine's listeners, before the new state's Enter).
    // Carries the process's IO and CPU counters (HasCounters).
    State,
    // The startup playlist was written (Name: the entry's name and how many names it holds).
    PlaylistWritten,
    // The client's movie player started a video (Name) / stopped playing.
    VideoStarted,
    VideoStopped,
    // The loading screen manager's active load state changed (Name: its vtable, module+0xRVA, or none).
    LoadState,
    // The loading screen manager became visible / hidden.
    LoadingScreenShown,
    LoadingScreenHidden,
    // The client game state machine's fade phase (ecl::GameStateMachine+0x28) changed, From -> To:
    // 0 none, 1 fading out, 2 black, 3 fading in.
    Fade,
    // A state of the interface's state machine (ui::UIStateMachine) appeared (To = 1) or went
    // away (To = 0): Name, Parent (the state it hangs under), From = its depth (Root = 0).
    UIState,
    // A step of the engine's own start, before the first game state: Name is the game function
    // (or the extender's), From its phase (StepBegin, StepEnd, StepAt). Carries the counters.
    Step,
    // A file the engine opened through ls::FileReader before the client's first game state
    // (Name: the end of its path). Only the first MaxFileRows are recorded; every opening is
    // counted in FileOpens.
    FileOpened,
    // The loading screen manager's progress changed (ls::LoadingScreenManager +0x30..+0x48):
    // From = Progress, To = ProgressTotal, Name = "LocalProgress/LocalProgressTotal".
    LoadingProgress,
    // The loading screen drew a hint (gui::DCLoadingScreen::OnAdvanceHintCommand): Name = its
    // HintKey, From = its index in ecl::HintManager's list, To = the list's count.
    HintShown,
    // The mods' hints were composed into the game's list (Extender/Client/ModHints.h): From = the
    // game's count, To = the count after, Name = how many mods' files were found.
    HintsComposed,
    // A client timeline's eoc::TimelineDataComponent.PlaybackState changed: Name = the timeline's
    // resource (ecl::timeline::TimelineLoadedComponent.Timeline), Parent = its TimelineType,
    // From -> To the states (-1: the timeline entity appeared / went away).
    TimelinePlayback,
    // The loading screen's slideshow (gui::LSImageSequence) was made ready by its OnLoaded: one row
    // per image of its set in the order it will show them (shuffled, when the control shuffles):
    // From = the position (1..), To = the set's size, Name = the image (its UriSource's file name
    // without the extension), Parent = "<slot>/<fade in>/<fade out>" in milliseconds (the control's
    // +0x290, +0x288 and +0x298: each image's time, and its fades inside that time). Owner,
    // OwnerData, Visible: the control's widget.
    WallpaperSet,
    // The wallpaper a live element shows changed: the Image.Source of a slideshow (set by its
    // storyboard's clock) or of the single image of the loading screen's other looks (an Image, a
    // Control's template root, whose source is under Assets/LoadingScreen/), read on the
    // interface's update for as long as that element lives (the widget outlives
    // LoadingScreenHidden by seconds). Every live element is followed, each with its widget (Owner,
    // OwnerData, Visible), so the game's loading screen and a mod's page are told apart; a newer
    // element of the same widget supersedes the older. From = the position in the set (1..; 0 for
    // a single image), To = the set's size (1 for a single image), Name = the image, Parent =
    // "sequence" or "single".
    WallpaperShown,
    // The wallpaper element left the interface (nothing but the record holds it any more: its
    // widget is gone): From, To, Name, Parent as the last WallpaperShown of that element.
    WallpaperGone,
    // The mods' interface merged (Extender/Client/EarlyModUI.h). Parent "early": the merge block
    // run at the first StartLoading, From = the modules given to ui::ModManager::RegisterMods, To =
    // its answer (1 true, 0 false; -1 not run, -2 the game's code faulted), Name = their folders or
    // why it was not run. Parent "game": the game's own merge is pending on this update (From = the
    // client's LoadOrderedModules); Name then says whether that set is the early merge's.
    EarlyModInterface,
    // The game's module list was taken for the early readers (Extender/Client/GameModules.h): From =
    // its modules, To = modsettings.lsx's entries, Name = where it came from ("LoadModFromSettings",
    // "LoadOrderedModules", "Settings") and, at a LoadModule's end, whether it is the list taken
    // before, Parent = the moment ("StartLoading", "LoadModule").
    ModuleList
};

constexpr int32_t StepAt = -1;
constexpr int32_t StepBegin = 0;
constexpr int32_t StepEnd = 1;

constexpr uint32_t MaxFileRows = 384;

struct Event
{
    double Ms;
    Side EventSide;
    What Kind;
    bool HasCounters;
    int32_t From;
    int32_t To;
    // The process's counters so far (State, Step and ExtenderLoaded rows): bytes read, read
    // operations, CPU time (user + kernel, all threads), and the files ls::FileReader opened
    // since the extender's hook on it.
    uint64_t IoReadBytes;
    uint64_t IoReadOps;
    double CpuMs;
    uint32_t FileOpens;
    char Name[64];
    char Parent[32];
    // The wallpaper rows' element (WallpaperSet, WallpaperShown, WallpaperGone): the widget it lies
    // in - its nearest ancestor that is a ls.UIWidget, by x:Name and the type of its DataContext
    // (empty when it lies in none) - and whether it is visible (its IsVisible; 1, 0, or -1 not read).
    char Owner[40];
    char OwnerData[40];
    int8_t Visible;
};

// Enough for a start, the main menu, several loads and the timelines of a session (a start to the
// main menu takes about 600 rows, most of them the engine's steps and files before the first state).
constexpr uint32_t Capacity = 8192;

void Record(Side side, What what, int32_t from = -1, int32_t to = -1, char const* name = nullptr,
    char const* parent = nullptr);

// Milliseconds since the process was created: the clock of every row.
double NowMs();

// Record with the process's IO and CPU counters (GetProcessIoCounters, GetProcessTimes).
void RecordWithCounters(Side side, What what, int32_t from, int32_t to, char const* name = nullptr);

// A step of the engine's start (What::Step), with the counters.
void RecordStep(char const* name, int32_t phase);

// An opening of a file by ls::FileReader (the extender's wrapper of its constructor; any thread).
void FileOpened(char const* path, std::size_t length);

// Polls the client's movie player, the loading screen manager and the fade phase of the client
// game state machine (`machine`, the ecl::GameStateMachine whose Update just ran); called once per
// client tick, on the thread that runs that Update.
void PollClient(void* machine);

// Polls the interface's state machine; called on the interface's update (the existing pre-hook of
// ui::deferred::PredicatesManager::UpdateFromUI, where the extender touches Noesis).
void PollUi();

// Around gui::DCLoadingScreen::OnAdvanceHintCommand (`screen` is the view model): the hint it drew,
// if it drew one, becomes a HintShown row. Called on the thread that runs the command.
void BeforeAdvanceHint(void* screen);
void AfterAdvanceHint(void* screen);

// After gui::LSImageSequence::OnLoaded (`sequence` is the control): its set becomes WallpaperSet
// rows, and the control is followed (one reference) for PollUi's WallpaperShown rows. Any thread.
void AfterImageSequenceLoaded(void* sequence);

// Events recorded so far (in the order their slots were taken) and how many were dropped.
uint32_t Count();
Event const& Get(uint32_t index);
// Whether the event at that index is written (a slot is taken before it is filled, on any thread).
bool IsReady(uint32_t index);
uint32_t Dropped();

END_NS()
