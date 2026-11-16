# GateKit's fork of the Script Extender

This is the [GateKit](https://github.com/bg3gk/gatekit) organisation's fork of Norbyte's Script
Extender for Baldur's Gate 3; for everything about the Script Extender itself - what it is, how to
install it, its API and its documentation - go to [Norbyte/bg3se](https://github.com/Norbyte/bg3se).

**For developers and tooling.** A player wants upstream's release, not this.

## What it adds

- **Instruments for reading the engine** (developer mode only): the memory instrument in
  `Ext.Debug` - field and component addresses, `module+0xRVA` conversion, checked reads and writes,
  hardware watches that say which instruction touches a field, freezes, crash dumps of our own; and
  the entity tracer in `Ext.Entity` with filters by entity and component, counts and ordered events,
  and which system changed a component; addresses of objects, properties and array elements;
  probes on any function of the game; and coverage counters that say which functions run.
- **Roll events for mods** (`Ext.Events.BeforeRoll`, `Ext.Events.Roll`): every check, attack,
  saving throw and damage roll can be seen and changed, as damage can.
- **More of the engine reachable from Lua**: many more components mapped, under the game's own
  names, with the older names kept as aliases.
- **What GateKit needs to drive the game**: a frame taken from inside, the frame mirrored to another
  program, the engine's own frame cap, a window that stays in the background, the sound settings
  mapped.

The detail is in [GATEKIT.md](GATEKIT.md); what uses it is [GateKit](https://github.com/bg3gk/gatekit).

## Upstream

Based on upstream's v34 line and following it: `gatekit-v34` is the live branch, `main` tracks
upstream.

## Building

`gatekit/setup-externals.cmd` fetches what the build needs, `gatekit/build.cmd` builds
`BG3ScriptExtender.dll` (`Game Release`, x64).

## Licence and credit

The Script Extender is Norbyte's work. The licence ([LICENSE](LICENSE)) and every notice are
upstream's, kept as they are. The changes here are offered back upstream where they fit.
