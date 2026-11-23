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
    FileOpened
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
};

constexpr uint32_t Capacity = 1024;

void Record(Side side, What what, int32_t from = -1, int32_t to = -1, char const* name = nullptr,
    char const* parent = nullptr);

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

// Events recorded so far (in the order their slots were taken) and how many were dropped.
uint32_t Count();
Event const& Get(uint32_t index);
uint32_t Dropped();

END_NS()
