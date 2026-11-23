#pragma once

// GateKit addition (the switch "LoadTrace", off by default): what the game does in every moment of
// a load, for measuring. With the switch on, from the extender's first moment:
// - every file the engine opens through ls::FileReader (its logical path, the time the open took,
//   the size it gave, the thread);
// - every file the process opens through the system (CreateFileA/W, CreateFile2 of KernelBase) and
//   every ReadFile on it (bytes, time, thread): the physical reads, packages included;
// - at every game state of the client and the server: the process's memory, IO and the CPU time of
//   each of its threads;
// - a sampling profiler: every LoadTraceSampleMs the call stack (functions' starts, by the modules'
//   unwind tables) of every thread that used CPU since the last sample, and always of the game's
//   state threads (so a wait is seen too).
// Nothing is changed in the game. Ext.StartupVideos.LoadTraceDump writes it out, summed per client
// game state (Extender/Shared/LoadTrace.inl).

#include <cstdint>
#include <string>

BEGIN_NS(load_trace)

// Called once from ScriptExtender::Initialize when the switch is on: the hooks and the sampler.
void Install(uint32_t sampleMs);
bool Enabled();

// A game state entered (side 1 client, 2 server; the state's numeric value), on the state's thread.
void OnState(int side, int32_t from, int32_t to);

// The calling thread has this role (a constant string), once per thread.
void Role(char const* role);

// Around the engine's ls::FileReader constructor (the extender's wrapper).
double FileReaderBegin();
void FileReaderEnd(double beganMs, char const* path, std::size_t length, uint32_t mapping, uint32_t flags,
    uint64_t size, bool loaded);

// Writes the record since `sinceMs` (process milliseconds; 0 = everything) into `path` (and
// path + ".opens.tsv", ".reads.tsv", ".samples.tsv"); returns what was written or why not.
std::string Dump(char const* path, double sinceMs);

// Forgets everything recorded so far (call it while nothing loads).
void Reset();

// The server's state worker (esv::GameStateThreaded::GameStateWorker, as its DoWork gets it): its
// tick period in microseconds at +0x90 (DoWork ticks when that much time passed, and its
// ls::PreciseClock at +0x78 waits out the rest of the period: 0 = no wait, a tick every loop).
// Known once the worker started; with the switch LoadTrace only.
void ServerWorker(void* worker);
// The period now (-1: no worker known); `setTo` >= 0 writes it first.
int64_t ServerTickUs(int64_t setTo);

// The client's PrepareRunning waits on an ls::StreamingPoller (the state +0x20, as the Toolkit's
// ecl::GameStatePrepareRunning::Update passes it to ls::StreamingPoller::Poll): +0 the least wait,
// +4 the most (seconds), +8 the time waited, +0xC the polls in a row that found nothing streaming.
// It ends when the time waited passes the most, or passes the least after 7 quiet polls. Called on
// every client update after the machine's: while the client is in PrepareRunning the poller is
// read (the first and last values kept), and when a most-wait override is set (>= 0) it is written.
void OnClientUpdate(void* machine, bool prepareRunning);
// The override in seconds (< 0: none, the game's own wait); returns the one before.
float PrepareRunningMaxWait(float seconds, bool set);
// The poller as last read: least, most, waited, quiet polls; and as first read in this state.
struct PollerSeen { float Min, Max, Waited; int32_t Quiet; float FirstMin, FirstMax; int Reads; };
PollerSeen LastPoller();

END_NS()
