#pragma once

// GateKit addition: loads without the game's waits, for automated runs (tests), each part behind
// its own switch in ScriptExtenderSettings.json, all OFF by default. Measured by W3 (start-zone
// night): an in-session load is bound by the ticks of the client and the server, not by its work.
// - FastLoadClientFrames: the client's frame caps lifted (0, BaseApp's caps as
//   Extender/Client/FrameCap.inl writes them) from the client's StartLoading until it is in
//   Running or Menu again, then put back as they were.
// - FastLoadServerTick: the server worker's tick period (esv::GameStateThreaded::GameStateWorker
//   +0x90, 33333 us) set to 0 when the server enters a loading state, put back when it is Running.
// - FastLoadStreamingWait: the client's PrepareRunning ends at its first update: its
//   ls::StreamingPoller's least and most wait (state +0x20, 0.5 s and 20 s) set to 0. Textures go on
//   streaming in Running (a picture taken at once can be unfinished).
//   FastLoadStreamingGate (W3 round 5) with it: only the least wait set to 0, the most wait kept -
//   ecl::GameStatePrepareRunning::Update also waits, inside its most wait, for the root level's start
//   dialogue (eoc::ServerRootLevelStartDialogComponent without ecl::ClientRootLevelStartDialogReady
//   Component and a pending count): with the most wait 0 that gate is passed at once and the story's
//   arrival (a start dialogue, a scripted death) lands after LevelGameplayStarted, not before it as
//   natively. With the gate kept the state ends when the dialogue is ready and 7 polls are quiet.
// - FastLoadFades: GlobalSwitches FadeSpeed (the screen fade's seconds, 0.5) set to 0: the fades
//   of the client's state machine around a load take one frame. The game's other fades that read it
//   are instant too.
// Nothing is called that the game does not call; only these numbers are written, on the thread
// that reads them. Every application is a timeline row (Extender Step "FastLoad ...").

#include <cstdint>

BEGIN_NS(fast_load)

void Configure(bool clientFrames, bool serverTick, bool streamingWait, bool fades, bool streamingGate = false);
bool Any();

// The client's thread: a client game state entered (the listener, before Enter).
void OnClientState(int32_t from, int32_t to);
// The client's thread: after ecl::GameStateMachine::Update; `prepareRunning` when the client is in
// PrepareRunning (`machine` is the ecl::GameStateMachine, CurrentState at +0).
void OnClientUpdate(void* machine, bool prepareRunning);

// The server's state worker (DoWork's argument), on its thread before DoWork.
void OnServerWorker(void* worker);
// The server's thread: a server game state entered.
void OnServerState(int32_t from, int32_t to);

END_NS()
