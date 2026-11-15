# This fork

A fork of [Norbyte/bg3se](https://github.com/Norbyte/bg3se), the Baldur's Gate 3 Script
Extender, maintained for [GateKit](https://github.com/bg3gk/gatekit). Norbyte's Script Extender
is the foundation everything here stands on; this fork exists to see more of the engine from
Lua, not to replace it.

**Licence is unchanged**: the Commons Clause condition followed by MIT, as upstream, in
[LICENSE](LICENSE). Copyright for the original work remains Norbyte's.

**Branch**: `gatekit-v34`, on top of upstream's `imgui_v1_93` (base commit `2ea16d51`). It tracks
upstream and is rebased onto it rather than carrying a long-lived divergence.

## What it adds

- **Component coverage.** Far more of the engine's own ECS components are exposed to Lua, with
  their fields named. The work is a loop: read the layout from the game's own code, declare it,
  then check in a running game that what Lua returns is what the raw memory holds.
- **The game's own names.** Where the game has a name for a field, that name wins; the previous
  name stays as an alias, so existing scripts keep working. Several of upstream's names were
  found to describe the wrong thing (a death flag that is really "instant death", a frame delta
  read as an integer) and are corrected this way, each confirmed in a live game before changing.
- **Fixes that came out of using it hard**: layouts that were the right size but the wrong order,
  structures a few bytes short, properties that returned garbage.
- **A picture of the frame, taken from inside** (`Ext.ClientRender.Screenshot`): copied out of the
  swap chain before the extender draws anything of its own, so the picture is the game alone, and
  taken without the window being in front or even visible.
- **The frame itself, handed to another program** (`Ext.ClientRender.MirrorEnable`): the finished
  frame is copied on the graphics card into a shared texture rather than brought back into memory,
  which is what lets a window elsewhere show the game at the game's own rate. The cost to the game
  is one GPU-to-GPU copy a frame, measured at 149.2 against 149.9 frames a second with it off.
- **The frame cap the engine already had** (`Ext.ClientRender.SetFrameCap`): `BaseApp` keeps the
  caps and `ls::GameClock` the microseconds; this writes the same fields the engine's own setter
  writes, so a copy nobody is watching draws at the rate it was given. Nothing hooked, nothing
  patched, and it refuses to write at all if the layout it checks for has moved.
- **A copy that does not take the keyboard** (`KeepWindowInBackground`,
  `Ext.ClientRender.KeepInBackground`, `Ext.ClientRender.ForegroundLog`): a game started by a tool
  should not throw its window over the person's work. Attempts to bring the window forward are
  refused, and - the half that actually mattered - the window is given `WS_EX_NOACTIVATE`, because
  Windows activates a window with nobody calling anything: the first top-level window of a process
  when it is shown, and the next window in line when the one that had the keyboard goes away.
  Every attempt is recorded with the module and offset of whoever made it, whether it is refused
  or not, which is how we learned that this game never asks: over a whole eighty-second start the
  record was empty. Off by default - a game somebody started to play may raise itself.
- **A memory instrument, developer mode only** (`Ext.Debug`): the address of a component's field
  for an entity, from the extender's own type information (`GetFieldAddress`); conversion both ways
  between an absolute address and `module+0xRVA` (`ModuleFromAddress`, `AddressFromModule`); a
  bounds-checked read and write of a typed value (`ReadValue`, `WriteValue`, a write refused on a
  code page); and **who writes a field** - a hardware watchpoint (`WatchWrite`) on up to four
  addresses, armed on every thread through the debug registers, caught by a vectored handler that
  records the writing instruction and a short return chain as raw addresses into a ring buffer
  (`ReadWatchHits`), resuming at once. The handler does no allocation; exceptions that are not ours
  are passed on; threads made after arming are picked up by re-arming, throttled to once a second so
  the game's tick rate is not disturbed. A value can be held fixed (`Freeze`, rewritten each tick).
  Everything is bounds-checked against committed memory; nothing names a function in the extender -
  the caller turns `module+0xRVA` into a name through its own code database.
- **The instruments made sound and completed** (`v34-instruments`): a watch on reads no longer
  closes the game (the handler reads through the kernel and stays installed, so clearing never
  leaves a thread unanswered); a hit's `Before` is a copy taken at arming and after every hit, and
  its address is named `NextInstruction`, since a data breakpoint fires after the access. Watches
  and freezes belong to the side (server or client) that made them - `ListWatches`, clearing by
  owner - and are served from the extender's own tick, so a Lua reset or a load leaves none unserved;
  hits carry the same ECS frame and flush counters as the tracer's rows. The tracer fills each
  entry's `Entity`, gives flags also as one `FlagMask`, takes its options as optional and component
  names as the engine's or as Lua labels (an unknown name is an error), filters by `Entities` and
  `Components` where the changes are read, counts each entry (`Count`, `LastFrame`, `LastFlush`)
  with a bounded list of ordered `Events`, and with `TrackSystems` says which system was updating
  when a filtered component changed. Component event and system update hooks take the Lua lock
  before looking a subscription up, and defer changes made during a dispatch. Crash dumps can go to
  a folder of our own, one set per crash, with the faulting `module+0xRVA` in a text file beside
  the dump and the reporter window optional. Not in this build: property sizes and paths below the
  top level, a watch over a range, addresses of objects behind any proxy, and a search by value.

## Settings this fork adds

In `ScriptExtenderSettings.json`, beside upstream's:

| Setting | Default | What it does |
|--|--|--|
| `KeepWindowInBackground` | false | Refuses the game's attempts to bring its window forward |
| `KeepWindowOffScreen` | false | Puts the window where no screen reaches when it is created |
| `ShiftOpensConsole` | true | Holding Shift at start opens the console |
| `CrashDumpDirectory` | empty | Folder for crash dumps, `<pid>-<yyyymmdd>-<hhmmss>.dmp` and a `.txt` with the faulting `module+0xRVA`; when set, crash reporting is on even with `SendCrashReports` off |
| `LaunchCrashReporter` | true | Starts the reporter window after a dump |

Every change of meaning — not only of layout — is verified in the running game before it lands.
What is verified and what is merely deduced is recorded with the change.

## Upstreaming

These are improvements to Norbyte's work, and belong back in it where he wants them. Nothing
here has been offered upstream yet; when it is, it will be as ordinary pull requests against
`Norbyte/bg3se`, one subject at a time, with the evidence for each change.

## Building

As upstream: Visual Studio with the C++ toolchain, `BG3Tools.sln`, the `Game Release`
configuration. GateKit users do not build it — `gatekit se install` fetches a release.

**Build `v34-tools`** (2026-11-14, normal mode with whole-program optimisation, `Game Release`
x64, from `fd2239ff`): `gatekit-v34` with the memory instrument merged (`Ext.Debug`, above), the
ECS tracer stamps merged (each change in `Ext.Entity.GetTrace()` carries `Frame` and `Flush`, and
the log itself the running counters) and `GlobalSwitches::SoundSettings` mapped. DLL SHA-256
`2b983653949938cdcb0044e157dc41dd1e03d2abd0457f948900343cc1eb95bf`.

**Build `v34-instruments`** (normal mode, `Game Release` x64, from `7b9b3ad1` on
`gatekit-v34-instruments`): the instruments above. DLL SHA-256
`812f1c948d5a2e62049840bfc38d6e3281c5c2110648d88a75f13259fae1be78`.
