#pragma once

// GateKit addition (W3 round 3, start-zone night), for automated runs and for measuring; every part
// behind its own switch in ScriptExtenderSettings.json, all OFF by default.
// - RenderCut: the main thread's frame, ls::RenderThread::Update (reached from WinMain's loop
//   through BaseApp::OnIdle), wrapped so that its picture can be left out: before the update the
//   render frames of ls::RenderFrameSystem are turned off (their enabled byte), so the update
//   begins, records, submits and presents none of them - and does all the rest as the game's: its
//   waits for the client's frame, the window's messages, the tasks other threads dispatch to the
//   main thread, the resource manager and the render commands. (Skipping the whole update was tried
//   first: the client thread waited for the main thread forever.) The frames turned off are turned
//   on again when drawing comes back. Mode 0 draws (the game's own), 1 draws nothing, N >= 2 draws
//   one update in N. RenderCutSkip: mode 1 from the start; else Ext.StartupVideos.RenderCut(1).
// - FastLoadLuaGC: the game's own embedded Lua 5.3 (the AI behaviour scripts of ls::anubis::System,
//   which runs them at every session load on the server's thread) has its collector held while the
//   server is in a loading state: its step (unplaced::luaC_step) on the server's thread sets the
//   state's debt below zero instead of collecting, as Lua 5.3's own step does for a stopped
//   collector (-GCSTEPSIZE*10 bytes); the step comes back after that many bytes, and once the server
//   has left the loading states it collects as the game's. No state is remembered between steps.
//   Measured: ~30% of the server thread's samples while it loads a session are
//   that collector. Lua's collector is invisible to the scripts' logic (no finalizers are run early
//   or late in a way a behaviour tree reads); memory grows during the load by the garbage made.
// - RenderCutSkipPaceUs (W3 round 7): a skipped update returns at once, so with nothing drawn the main
//   thread loops ~1,600 updates a second through a start or a load. With a pace of N us the wrapper
//   sleeps (a high-resolution waitable timer) after a skipped update until N us after the previous
//   one began; a drawn update is never paced. 0: off. RenderCutSkipPaceStartOnly ends pacing at
//   the client's first Running so in-session loads keep their unpaced timing.
// Counters for both (frames drawn and skipped, the client's and the server's ticks, each thread's
// CPU time, the process's memory) are read by Ext.StartupVideos.RenderCut().

#include <cstdint>

BEGIN_NS(render_cut)

// Inside ScriptExtender::Initialize's Detours transaction: the wrappers the switches ask for.
// skipFromRunning (W3 round 5): mode 1 set at the client's first Running instead of at the start.
void Install(bool renderCut, bool skipFromStart, bool luaGc, bool skipFromRunning = false, uint32_t skipPaceUs = 0,
    bool paceStartOnly = false);

// The client's thread: a client game state entered (the first Running starts the skip of
// RenderCutFromRunning).
void OnClientState(int32_t from, int32_t to);

// The skip mode (see above); a negative mode only reads. Returns the mode before.
int SetMode(int mode);

struct Counters
{
    bool Installed;
    bool LuaGcInstalled;
    int Mode;
    uint64_t Frames;        // ls::RenderThread::Update calls that reached the wrapper
    uint64_t Skipped;       // of them, with the render frames turned off
    uint32_t RenderFrames;  // the render frames of ls::RenderFrameSystem now
    uint32_t RenderFramesOff;   // of them, turned off by this
    uint64_t ClientTicks;   // the client's updates (ecl::GameStateMachine::Update)
    uint64_t ServerTicks;   // the server's updates
    uint64_t GcHeld;        // collector steps held on the server's thread
    uint64_t GcReleased;    // loads at whose end the collector was given back
    double ClientCpuMs, ServerCpuMs, MainCpuMs, ProcessCpuMs;
    double Ms;              // the process clock (the timeline's)
    double PrivateMB, WorkingSetMB;
    uint32_t SkipPaceUs;    // RenderCutSkipPaceUs (0: off)
    uint64_t PacedSleeps;   // skipped updates the pace slept after
    double PacedSleptMs;    // the time they slept
};
Counters Read();

// The client's thread, every update.
void OnClientTick();
// The server's thread, every update.
void OnServerTick();
// The server's thread: a server game state entered.
void OnServerState(int32_t from, int32_t to);

END_NS()
