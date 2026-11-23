#pragma once

// GateKit addition (W3 round 4, start-zone night), toward a headless game kept open at little
// cost; every part behind its own switch in ScriptExtenderSettings.json, all OFF by default.
// - IdleSleep: the game's tick wait (unplaced::TickWait, the wait both game threads call at the
//   end of a tick: the server's worker at +0x78, the client's game thread) takes the time it is
//   asked to wait - the period at +0x18 less the carry at +0x28 and its second argument - and
//   spins it out: QueryPerformanceCounter with SwitchToThread, running queued worker jobs between,
//   then a tight spin for the last millisecond. With the switch on, a wait of more than 3 ms first
//   sleeps on a high-resolution waitable timer until 1.5 ms before its end, and the game's own
//   wait does the rest, told (its second argument) how long was slept: the tick ends when the
//   game's would, its carry (+0x28) is the game's, and its measured time (+0x20) has the sleep
//   added. A wait of 3 ms or less (an uncapped tick, a load with FastLoad) is the game's untouched.
// - StubFileExtensions: the engine's opens (ls::FileReader) of a path ending in one of the listed
//   extensions are given a path that does not exist, so the reader comes back not loaded, as for
//   a missing file; the game's own fallback for a missing resource takes its place.
// Counters: Ext.StartupVideos.Headless().

#include <cstdint>
#include <string>

BEGIN_NS(headless)

// Inside ScriptExtender::Initialize's Detours transaction: the wrappers the switches ask for.
void Install(bool idleSleep, std::string const& stubExtensions);

// Whether an open of this path is stubbed (StubFileExtensions); counts it when it is.
bool Stubbed(char const* path, std::size_t length);

struct Counters
{
    bool IdleSleepInstalled;
    uint64_t Waits;          // tick waits that reached the wrapper
    uint64_t Slept;          // of them, with a sleep first
    double SleptMs;          // the time slept, measured
    uint32_t StubExtensions; // extensions listed
    uint64_t StubbedOpens;   // opens given a path that does not exist
};
Counters Read();

// GateKit (W3 round 6), the switch TreasurePin (off by default) installs a wrapper of
// esv::inventory::TreasureGenerationSystem::GenerateTreasure, inert until Lua pins a seed
// (Ext.Debug.PinTreasure). The game rolls every treasure table - a level's containers at its first
// visit, a character's loot at death - from one generator, the server's saved random (esv::EocServer
// +0xB8, ls::Random, seeded from a millisecond clock at the server's start), in the order the
// requests come, and that order follows the level setup's spread over ticks: the same seed gives
// the same loot only when the requests come in the same order. Pinned, each call first seeds the
// saved random from the seed and the entity's UUID, so an entity's loot depends on the seed and
// the entity alone. A cheat for differential checks, like a rigged die; an entity with no UUID
// keeps the shared sequence (counted).
struct TreasureCounters
{
    bool Installed;
    bool Pinned;
    uint32_t Seed;
    uint64_t Calls;     // calls of GenerateTreasure seen by the wrapper
    uint64_t Keyed;     // of them, seeded from the seed and the entity's UUID
    uint64_t Unkeyed;   // of them, pinned but the entity had no UUID (the shared sequence kept)
};
void InstallTreasurePin(bool on);
void PinTreasure(bool on, uint32_t seed);
TreasureCounters ReadTreasure();

// GateKit (W3 round 7), measuring only: the switch LargeAllocTraceMB (0, off by default) wraps
// kernel32's VirtualAlloc from ScriptExtender::Initialize on; a call that commits or reserves at
// least that many MB is written into the start's timeline (a Step row: the size, the address, the
// protection and the first return addresses as module+RVA), so the owner of an engine arena is
// named by its caller. The first 64 such calls only; nothing else changes.
void InstallLargeAllocTrace(uint32_t megabytes);

// Record self-termination by ExitProcess or TerminateProcess in exit-<pid>.txt, with recent
// exceptions and timeline events. The game's exception filter can terminate without a dump.
void InstallExitWitness(bool on, std::wstring const& folder);

END_NS()
