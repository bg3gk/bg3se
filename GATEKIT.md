# This fork

A fork of [Norbyte/bg3se](https://github.com/Norbyte/bg3se), the Baldur's Gate 3 Script
Extender, maintained for [GateKit](https://github.com/bg3gk/gatekit). Norbyte's Script Extender
is the foundation everything here stands on; this fork exists to see more of the engine from
Lua, not to replace it. This page is for a modder: what each addition is for, how to call it,
and where it stops.

**Licence is unchanged**: the Commons Clause condition followed by MIT, as upstream, in
[LICENSE](LICENSE). Copyright for the original work remains Norbyte's.

**Branches**: `gatekit-v34`, on top of upstream's `imgui_v1_93` (base commit `2ea16d51`), holds
the component coverage, the names, the memory instrument and the window and frame tools;
`gatekit-native` adds the rest of this page on top of it. Both track upstream and are rebased
onto it rather than carrying a long-lived divergence.

## What it adds

| Addition | Side | Developer mode |
|---|---|---|
| [Component coverage and the game's names](#component-coverage-and-the-games-names) | both | no |
| [Roll events](#roll-events): `Ext.Events.BeforeRoll`, `Roll`, `RollAdjusted` | server | no (`RollDie`, `Caller`: yes) |
| [The game's own chance of a roll](#the-games-own-chance-of-a-roll): `Ext.Stats.GetSpellRollChance` | server | no |
| [The game's functions by the registry's names](#the-games-functions-by-the-registrys-names): `RegistryMappings.xml`, `Ext.Debug.GetSymbolAddress`, the C entries | both | Lua: yes; C: no |
| [Addresses](#addresses): `Ext.Debug.GetObjectAddress`, `GetPropertyAddress`, `GetElementAddress`, field sizes | both | yes |
| [Function probes](#function-probes) and [coverage](#coverage): `Ext.Debug.AddProbe`, `StartCoverage`, ... | both | yes |
| [The memory instrument](#the-memory-instrument): watches, reads, writes, freezes, the tracer | both | yes |
| [A figure on the client only](#a-figure-on-the-client-only): `Ext.ClientVisual.CreateDummy`, `DestroyDummy` | client | no |
| [Animation and effects on the client only](#animation-and-effects-on-the-client-only): `Ext.ClientVisual.PlayAnimation`, `StopAnimation`, `PlayEffect`, `StopEffect` | client | no |
| [Injected input](#injected-input): `Ext.Input.Inject*` reach the game; `Injected` on input events | client | no |
| [Values of the interface](#values-of-the-interface): `Nullable<Bool>` and `TimeSpan` through `Ext.UI` | client | no |
| [Screenshots, the frame and the window](#screenshots-the-frame-and-the-window): `Ext.ClientRender.Screenshot(path, withOverlay)`, `MirrorEnable`, `SetFrameCap`, `KeepInBackground` | client | no |
| [A picture from the disk](#a-picture-from-the-disk): `Ext.IMGUI.LoadImageFile` | client | no |
| [Startup videos](Docs/API.md#startup-videos): a mod's `ScriptExtender/StartupVideos.json` hides, replaces and adds the videos the game plays at start, composed with every other mod's; `Ext.StartupVideos.GetPlaylist`, `Compose`, and `Timeline` (every stage of the start with its time, recorded natively before Lua exists: both sides' states with the process's IO and CPU counters, the videos, the loading screen, the client's screen fades and the interface's states up to the "press any key" page; and after the start: the loading screen's progress, the hints it draws and its wallpapers - the slideshow's shuffled set as `gui::LSImageSequence::OnLoaded` makes it, each image as the slideshow's clock shows it, the single images of the other looks - every client timeline's playback state - the main menu's camera descent among them - through the menu, later loads and a session) | client | no |
| [The mods' interface on the first loading screen](Docs/API.md#the-mods-interface-on-the-first-loading-screen): the game's own merge of the mods' interface (`ui::ModManager::RegisterMods`, the texture metadata, `gui::GameUI::ResetModdedLibs`, `ui::StateMachine::ApplyMods`) run once more at the first `StartLoading`, with the game's module list (the very set the game's own merge gets later), before the loading screen's widget is made - a mod's `BusyElement`, styles and `Loading` pages are on the very first loading screen; an `EarlyModInterface` timeline row for it and for the game's own merge; the wallpaper rows follow every live slideshow and single image, each with its widget (`Owner`, `OwnerData`, `Visible`) | client | no |
| [Loading hints](Docs/API.md#loading-hints): each loaded module's `Public/<Folder>/Hints/Hints.lsx` (the game's format) is loaded by the game's own `ls::FileFormatIO::LoadFile` and composed into the game's hint list - new keys appended, a known key replaced in place, the later mod winning with a warning; composed again when the game reloads its modules after a change of the mod list (the game's file loaded anew, every file of the new list), so a disabled mod's hints go and an enabled one's come; `Ext.Hints.GetComposition` | client | no |
| [The game's module list](Docs/API.md#which-modules) for every early reader (startup videos, hints, the early interface merge): at the first `StartLoading` the game's own resolver (`ls::ModManager::LoadModFromSettings`) is called once, and the readers read the load-ordered modules it leaves - the list the game then loads, dependencies and the game's own modules included, in its order (fallback: `modsettings.lsx`'s order, said so); a `ModuleList` timeline row at that moment and at each client `LoadModule`'s end; `Ext.StartupVideos.GetModuleLists` | client | no |
| [The game's mod list](Docs/API.md#mods): `Ext.Mod.GetList` (the settings in order, the modules the game loads with why - enabled, a dependency, the game's own - and the available ones, each with its dependencies and dependants), `Ext.Mod.Pending` (the manager's byte: whether the next session's start loads the modules again, and what changes), `Ext.Mod.SetEnabled` and `SetOrder` through the game's own mod manager calls (`ecl::ModManagerClient::AddMod`, `RemoveMod`: settings saved, list resolved, modules loaded at the next session's start); main menu only | client | no |
| [A save from the command line without the main menu](Docs/API.md#a-save-from-the-command-line-without-the-main-menu): with `DirectSaveLoad` (off by default), once the game's `Init` has queued the save's load (`-loadSaveGame`, `-continueGame`) behind the main menu's states, those states (`LoadMenu`, `StopLoading`, `Menu`) are taken out of the client's queue; the save is loaded by the game's own start of a save, ~17 s sooner; a `Step` timeline row says what was done | client | no |
| [Command-line values in any alphabet](Docs/API.md#command-line-values-in-any-alphabet): a value the game reads through `ls::ArgumentString::GetValue` is given in UTF-8 from the wide command line when that is all it changes - a save folder named in any alphabet is found by `-loadSaveGame` (`WideCommandLine`) | both | no |
| `Ext.Debug.ReadValue(address, "FixedString")`: a FixedString's text from its 4-byte index (read only: `WriteValue` and `Freeze` refuse it) | both | yes |
| [Fixed](#fixed): `GenomeVariant` values larger than a pointer | both | - |
| [Settings this fork adds](#settings-this-fork-adds) | - | - |

Every change of meaning, not only of layout, is verified in the running game before it lands;
what is verified and what is merely deduced is recorded with the change.

## Component coverage and the game's names

Far more of the engine's own ECS components are exposed to Lua, with their fields named. A
layout is read from the game's own code, declared, then checked in a running game: what Lua
returns is what the raw memory holds.

Where the game has a name for a field, that name wins; the previous name stays as an alias, so
existing scripts keep working. Several of upstream's names describe the wrong thing (a death
flag that is really "instant death", a frame delta read as an integer) and are corrected this
way, each confirmed in a live game before changing. Layouts that were the right size but the
wrong order, structures a few bytes short and properties that returned garbage are fixed.

## Roll events

For a mod that wants to see or change any roll, as it can damage, and learn what the roll is for
and what an interrupt did to it afterwards. Every roll the game's roll manager makes - a check, an
attack, a saving throw, damage, a heal, initiative - raises two server events, as dealing damage
raises `DealDamage` and `DealtDamage`: one pair per roll as the player understands it, whichever
of the roll manager's functions carried it out. Server only; no developer mode; an event is built
only when somebody listens.

```lua
Ext.Events.BeforeRoll:Subscribe(function (e)
    if e.RollType == "SkillCheck" then e.Advantage = true end
end)

Ext.Events.Roll:Subscribe(function (e)
    _D(e.Roller, e.RollType, e.Dice, e.Natural, e.Total, e.Source, e.Target, e.Spell)
    if e.RollType == "Attack" and e.Natural == 1 then e.Natural = 2 end
end)
```

| Field | `BeforeRoll` | `Roll` | |
|--|--|--|--|
| `Roller` | read | read | the entity that rolls (empty for dice no creature rolls: a heal's dice, initiative) |
| `RollType` | read | read | the game's `RollType` |
| `DiceType`, `DiceCount` | read | read | the dice of the formula |
| `Bonus` | **write** | read | the flat part of the formula |
| `Advantage`, `Disadvantage` | **write** | read | |
| `DC` | **write** | read | `nil` for a roll against no difficulty |
| `HasStream`, `Stream`, `StreamId` | read | read | the roll stream the dice come from (`RollStreamType`) |
| `Source`, `Target` | read | read | the two parties of what the roll is for: the caster or attacker and the target; for a status its cause and its bearer. Empty when the game does not say (initiative, a roll the story asks for) |
| `Spell`, `Status` | read | read | the spell and the status the roll belongs to, when it belongs to one |
| `Dice` | | read | every die thrown, in order (two for a d20: the second is used with advantage or disadvantage) |
| `Natural`, `Total`, `Discarded`, `Critical` | | **write** | a changed `Natural` moves `Total` with it and sets `Critical` by the game's rule, unless they are written too |
| `Forced` | | read | the outcome was decided before the dice (Karmic Dice) |

What is written before the roll holds for that roll only; the caller's own structures are put
back afterwards. `Source`, `Target`, `Spell` and `Status` come from the condition or the stats
functor the roll is thrown inside (a saving throw against Sacred Flame names the caster, the
target and the spell; the dice of a healing potion name its status).

A roll may still change after it is thrown: an interrupt sets it, adjusts it or rerolls it
(Portent, Shield, Lucky, Bardic Inspiration). `Roll` always comes before that; `RollAdjusted` is
raised once the change is written:

```lua
Ext.Events.RollAdjusted:Subscribe(function (e)
    _P(e.OldNatural, "->", e.Natural, e.OldTotal, "->", e.Total, e.OutcomeChanged)
end)
```

Its fields: `OldNatural`, `OldTotal`, `OldCritical`, `Natural`, `Total`, `Critical`, `DC`,
`OutcomeChanged` (a failure became a success or the other way), `RollUuid` and `Roll`, the game's
`ConditionRoll` itself - the one the hit and the combat log are then built from (`nil` for a roll
that is no dice roll).

In developer mode `BeforeRoll` and `Roll` also carry `Caller` and `CallerChain`, the return
addresses into the game of the code that asked for the roll (`"bg3_dx11.exe+0x106CE8B"`), and
`RollDie` is raised for every single die (`Min`, `Max`, writable `Value`, `Caller`,
`CallerChain`).

**Limits.** Which interrupt changed a roll is not in `RollAdjusted`; a reroll by an interrupt
(Lucky) raises `RollAdjusted` only, not a new `Roll`; a change is not tied to its `Roll` by an id
(pair them by order).

## The game's own chance of a roll

For a mod that wants the number the game's combat AI weighs and its cursor shows, for any caster,
spell and target, without rolling anything.

```lua
local chance = Ext.Stats.GetSpellRollChance(caster, "Target_MainHandAttack", target)   -- 0.65
```

`Ext.Stats.GetSpellRollChance(caster, spell, target)` -> a number from 0 to 1, or `nil`: the
chance that the roll of `spell` cast by `caster` at `target` succeeds, advantage and disadvantage
folded in. `caster` and `target` are entities in the world, `spell` a spell's stats name. Nothing
is rolled and nothing changes. Server only; no developer mode. `nil` and an error line when the
spell or an entity is not there, when it is called on the client, or when the game's functions
were not found in this build of the game.

A weapon attack is worked out with the weapons of the caster's **active weapon set**
(`entity.WeaponSet.WeaponSet`), as the game does: asked for a melee attack while the ranged set
is active, it counts no melee weapon. The game switches the set when the attack is really made;
a mod that wants the chance of an attack not yet begun sets the set first and puts it back.

**Limits.** A spell's roll only: no chance for a skill check that is no spell's roll, and no
damage a hit would do.

## The game's functions by the registry's names

For a mod - in Lua or native, in any language - that needs the address of a function of the game
the extender declares no symbol for. GateKit keeps a registry of names for the game's machine
code and writes from it a table of byte patterns, `RegistryMappings.xml` (`gatekit registry
mappings write`): one pattern per function the registry names, each target naming the registry's
id (`esv::combat::System::CheckForSurprise`). When that file lies beside the extender's DLL, the
extender reads it after its own table and finds every pattern; `gatekit se install` puts it
there with the DLL. Without the file nothing changes. After a patch of the game the registry
re-anchors its names and the table is written again; nothing is searched by hand.

**From Lua** (developer mode, both sides):

```lua
local address, why = Ext.Debug.GetSymbolAddress("esv::combat::System::CheckForSurprise")
for _, s in ipairs(Ext.Debug.GetSymbolNames()) do
    if s.Source == "registry" and not s.Found then _P("not found: " .. s.Name) end
end
```

- `Ext.Debug.GetSymbolAddress(name)` -> the absolute address, or `nil` and why: a name of the
  registry's table, or one of the extender's own symbols (`stats__DealDamageFunctor__ApplyDamage`,
  the names of `GameHooks/DataLibrariesBG3Game.cpp`). A name whose pattern was not found in the
  running executable answers `nil` and says so.
- `Ext.Debug.GetSymbolNames()` -> `{ { Name, Found, Source = "extender" | "registry" } }`, sorted.
- `Ext.Debug.PinTreasure([seed])` -> `{ Installed, Pinned, Seed, Calls, Keyed, Unkeyed }`: with a
  seed, every treasure table rolled from then on is seeded from it and the entity's UUID (the
  switch `TreasurePin`); without one, the game's shared sequence again. Developer mode. W3 round 6.
- `Ext.Debug.MemoryMap([top])` -> `{ Totals = { Private, Mapped, Image = { Committed, Reserved } },
  Allocations = { { Base, CommittedMB, ReservedMB, Regions, Protect, Heap } }, Heaps,
  PrivateAllocations }`: the address space walked with `VirtualQuery`, in MB, the `top` (40)
  largest committed private allocations by allocation base (`Heap` when the base is a process
  heap) - what a memory map tool shows, from inside. Developer mode. W3 round 6.

**From native code**, no developer mode: three plain C entries of the DLL.

```c
#include <windows.h>

typedef void*       (*GateKitSymbolAddressFn)(const char* name);
typedef int         (*GateKitSymbolCountFn)(void);
typedef const char* (*GateKitSymbolNameFn)(int index);

HMODULE se = GetModuleHandleW(L"BG3ScriptExtender.dll");
GateKitSymbolAddressFn symbolAddress = se ? (GateKitSymbolAddressFn)GetProcAddress(se, "GateKitSymbolAddress") : NULL;
void* surprise = symbolAddress ? symbolAddress("esv::combat::System::CheckForSurprise") : NULL;
```

- `void* GateKitSymbolAddress(const char* name)`: the address of a name of the registry's table or
  of the extender's own symbols; `NULL` when unknown or not found.
- `int GateKitSymbolCount(void)`: how many names the registry's table holds (found or not).
- `const char* GateKitSymbolName(int index)`: the name at an index, sorted; `NULL` past the end.
  The pointer stays valid while the extender lives.

The names are filled when the extender has mapped the game; before that every answer is `NULL`
or 0. An entry missing from `GetProcAddress` means a build of the extender that is not this
fork's. Measured on game build 25605617: 279 names read, 279 found; the address the table gives
for the chance of a spell's roll is the one the extender's own hand-written pattern gives.

## Addresses

For finding where the game keeps what Lua shows, to read it raw, watch it or compare it with a
layout. Developer mode, both sides.

| Function | Answers |
|---|---|
| `GetObjectAddress(object)` | the address and kind of the game object behind a Lua reference (an object, an array, a map, a set), or `nil` and why |
| `GetPropertyAddress(object, name)` | `{ Address, Offset, Size, ObjectAddress }` of a property, or `nil` and why; `Size` is 0 for a property that is no plain field (a computed one) |
| `GetElementAddress(array, index)` | `{ Address, Size }` of an array's element; for an array of bits the word and `Bit`; `nil` and why for a collection that keeps no storage of its elements |

Every registered property carries the size of its field. A path such as
`Streams[2].State.NumRolls` is walked through Lua's own references and ends in one of these:

```lua
local health = Ext.Entity.Get(Osi.GetHostCharacter()).Health      -- any component
local at = Ext.Debug.GetPropertyAddress(health, "Hp")
_P(string.format("0x%X, %d bytes", at.Address, at.Size))
```

## Function probes

For seeing how a function of the game is called - its arguments, its callers, its answer -
and for bending it while a mechanic is studied, with no Lua on the game's thread. Developer mode,
both sides; a probe belongs to the side that set it.

| Function | |
|---|---|
| `AddProbe(address, [options])` | a slot (1-64), or `nil` and why. `Args` (integer arguments kept, 1-16, default 4), `Record` (false: count calls and callers only), `Return`, `Chain` (frames of the return chain, up to 32; `true` is 6), `Snapshots = { { Argument, Size } }` (at most two; up to 64 bytes behind a pointer argument, on entry and on return) |
| `SetProbeRule(slot, index, [rule])` | up to four rules (`index` 1-4; no `rule` clears one): `Kind` = `"Return"`, `"Argument"` or `"Skip"`, with `Value` (or `FloatValue`, `Float32Value`), `Argument`, `Count`, `WhenArgument` and `WhenValue`, `CallerFrom` and `CallerTo` |
| `GetProbeHits([max])` | the records since the last read, oldest first: `Slot`, `Sequence`, `Phase` (`"Entry"` or `"Return"`), `Depth`, `Thread`, `Time`, `Caller`, `Args`, `Doubles`, `Floats`, `Return`, `ReturnDouble`, `ReturnFloat`, `Rules`, `Chain`, `Snapshots` |
| `GetProbes([all])` | the probes with call counts, callers seen, rules and `OverheadTicks` |
| `RemoveProbe([slot])` | removes one probe, or all of the calling side |
| `GetProbeTestAddress()`, `CallProbeTest(a, b, c, d, e, f)` | a function of a known shape for checking the probes |

```lua
local slot = Ext.Debug.AddProbe(Ext.Debug.GetSymbolAddress("esv::combat::System::CheckForSurprise"), { Args = 2, Return = true })
-- ... play ...
for _, hit in ipairs(Ext.Debug.GetProbeHits()) do _P(hit.Phase, hit.Caller, hit.Args[1], hit.Return) end
Ext.Debug.RemoveProbe(slot)
```

A call costs about half a microsecond counted, one recorded, under twenty with the whole chain
and two snapshots. A function of more than sixteen parameters must not be probed; a rule that
skips a function or writes a wrong value can stop the game.

## Coverage

For learning which of very many functions run while the game is played, and called from where.
Developer mode.

| Function | |
|---|---|
| `StartCoverage(addresses, [hotLimit])` | `{ Asked, Placed, Refused }`; an address that is no function start is left out; a function called more than `hotLimit` times (default 200000) is taken off by itself |
| `GetCoverageStatus()` | `{ Functions, Attached, Hot, Seen, Calls, Pairs, Lost }` |
| `SaveCoverage(path, [reset])` | a text file: `F <module> <rva> <calls> <hot>`, `E <module> <rva> <module> <rva> <calls>`, `L <lost>`; answers `{ Functions, Pairs }` |
| `StopCoverage()` | takes every counter off and answers how many; the counts stay for a last save |

With a counter on every function of the executable of 64 bytes or more (about 176,000) a client
tick takes about a fifth longer; putting them on stalls the game for about a minute.

A function is in a record when it ran, whatever the ring lost: the counter marks its function as
seen besides writing the call into the ring, so `F` lines are complete and only the counts of the
calls and the pairs can fall short (`L`).

**The start, before any Lua.** The setting `CoverageAtStart` names a file of the executable's
function starts, one hexadecimal offset a line. The counters go on as soon as the extender has
started - its own hooks in place, the engine not yet started - which delays the start by the time
they take; at every state the client enters, up to `Menu`, the counts so far are saved beside the
file as `<file>.<NN>-<state>.txt` and started again. The counters stay on after the menu.

## The memory instrument

For finding who writes a field and holding a value fixed while a mechanic is studied. Developer
mode, both sides.

- The address of a component's field for an entity, from the extender's own type information
  (`GetFieldAddress`); conversion both ways between an absolute address and `module+0xRVA`
  (`ModuleFromAddress`, `AddressFromModule`); a bounds-checked read and write of a typed value
  (`ReadValue`, `WriteValue`, a write refused on a code page). `ReadValue` also reads a
  `"FixedString"`: the text of the index there, through the game's lookup under a guard (nil for
  an index the table does not hold).
- **Who writes a field**: a hardware watchpoint (`WatchWrite`) on up to four addresses, armed on
  every thread through the debug registers, caught by a vectored handler that records the writing
  instruction and a short return chain into a ring buffer (`ReadWatchHits`), resuming at once. A
  watch on reads does not close the game. A hit's `Before` is a copy taken at arming and after
  every hit, and its address is named `NextInstruction`, since a data breakpoint fires after the
  access. Watches and freezes belong to the side that made them (`ListWatches`, clearing by
  owner) and are served from the extender's own tick, so a Lua reset or a load leaves none
  unserved; hits carry the same ECS frame and flush counters as the tracer's rows.
- A value held fixed (`Freeze`, rewritten each tick).
- **The entity tracer** (`Ext.Entity.GetTrace()`): each entry's `Entity`, flags also as one
  `FlagMask`, component names as the engine's or as Lua labels (an unknown name is an error),
  filters by `Entities` and `Components`, a count per entry (`Count`, `LastFrame`, `LastFlush`)
  with a bounded list of ordered `Events`, each change stamped with `Frame` and `Flush`, and with
  `TrackSystems` the system that was updating when a filtered component changed.

Everything is bounds-checked against committed memory; nothing names a function in the extender -
the caller turns `module+0xRVA` into a name through its own code database. Component event and
system update hooks take the Lua lock before looking a subscription up, and defer changes made
during a dispatch.

**Not built:** a path below the top level resolved to an offset, a watch over a range, the address
of an object behind every kind of proxy, a search by value.

## A figure on the client only

For a mod that shows a character - in the main menu, in a scene of its own - that the server
knows nothing of.

```lua
local figure = Ext.ClientVisual.CreateDummy("8bdc382b-a31e-4c1c-8987-656ceb333f05", { 5.75, 0, 2.25 }, { 0, 0, 0, 1 })
-- a few frames later figure.Visual.Visual is there and the figure stands in the level
Ext.ClientVisual.DestroyDummy(figure)   -- true
```

`Ext.ClientVisual.CreateDummy(template, [position], [rotation], [timelineType])` -> the entity, or
`nil`. Client only, no developer mode. It builds a figure as the game builds the figures of a
cinematic that have no entity of the level behind them (the adventurers of the main menu, the
extras of a dialog): a timeline dummy made from a root template, by the game's own functions in
the game's order - `ecl::TLPreviewDummyHelpers::CreateDummy`, `SetupDummy`, `PrePrepareDummy`
(the template's look), `LoadDummy` - and, on the client tick after the visual has been loaded,
`AttachDummyToScene`.

- `template`: a **character** root template (the global template bank, as `Ext.Template.GetRootTemplate`).
- `position` `{x, y, z}` and `rotation` (a quaternion `{x, y, z, w}`): where the figure is put when its visual is loaded.
- `timelineType`: the game's `eoc::timeline::ETimelineType` (default 1, what a dialog's dummies carry).

The entity carries `TLPreviewDummy` (with `OriginalCharacterTemplate`), `Uuid`, `Transform`,
`Level`, `AnimationBlueprint` and, once loaded, `Visual` and `SceneAttach`. No timeline owns it,
so nothing moves or hides it: move it with `entity.Visual.Visual:SetWorldTranslate{x, y, z}` and
`:SetWorldRotate{x, y, z, w}`. It is no character (`ClientCharacter` is absent), it is not saved,
and `Ext.Entity.GetAllEntitiesWithComponent` does not list it - keep the entity or its uuid.

`nil` and an error line for a template that is not a character root template, with no level
loaded on the client (the main menu is one), or when the game's functions were not found.

`Ext.ClientVisual.DestroyDummy(entity)` -> `true`, or `false` for an entity that is no timeline
dummy: it removes the figure with the function the timeline system removes its own dummies with
(visual, physics and light unloaded, the entity destroyed at the end of the frame).

## Animation and effects on the client only

For a mod that makes a figure or a character play an animation, or plays an effect at a place,
with no server and no Osiris - in the main menu too. Client only, no developer mode.

```lua
-- an animation resource is linked into the figure's animation sets under its own id ...
local walk = "f427f411-ec56-4a0a-a921-68b591b080e8"
for _, w in ipairs(figure.AnimationWaterfall.Waterfall) do
    if tostring(w.Type) == "Visual" and tostring(w.Slot) == "" then
        Ext.Resource.Get(w.Resource, "AnimationSet").AnimationBank.AnimationSubSets[""].Animation[walk] = { ID = walk, flags = {} }
    end
end
Ext.ClientVisual.PlayAnimation(figure, { walk })                 -- ... and played once: true
Ext.ClientVisual.PlayAnimation(figure, {}, { fearLoop })         -- a loop, until it is stopped
Ext.ClientVisual.StopAnimation(figure)                           -- true

local fx = Ext.ClientVisual.PlayEffect("VFX_Cinematic_Orin_Teleportation_01", { 5.75, 0, 2.25 }, nil, 1.0, figure)
Ext.ClientVisual.StopEffect(fx, "Destroy")                       -- true
```

**How the game moves a figure.** A timeline dummy carries the same animation blueprint as a
character. A timeline drives it through the blueprint's timeline node; a figure no timeline owns
gets nothing from that. Gameplay drives the same blueprint with gameplay events, and the "play an
animation" action state of the client (`ecl::ASAnimation`, where `Osi.PlayAnimation` and
`Osi.PlayLoopingAnimation` end) sends two of them: `EoCGE::ASAnimation` to start and
`EoCGE::ASCancelAnimation` to leave.

`Ext.ClientVisual.PlayAnimation(entity, start, [loop], [finish], [time], [flag])` -> `true`, or
`false` for an entity with no `AnimationBlueprint` or when the game's function was not found. It
calls `ls::AnimationBlueprintSystem::TriggerGameplayEvent<EoCGE::ASAnimation>` for the entity: a
figure made with `CreateDummy` or a character.

- `start`, `loop`, `finish`: lists of animation short names, each may be empty. The blueprint
  plays one of `start`, then one of `loop` over and over, and one of `finish` when stopped. A
  short name is looked up in the entity's animation sets (`entity.AnimationWaterfall`): a
  resource a set does not link is linked in first, as above. The sets are shared resources: a
  link serves every entity that uses the set, until the game is closed.
- `time`, `flag`: the event's last two arguments as the game declares them (a float and a
  flag), passed as given; what the blueprint does with them is not known.
- **Its end** is read where the game reads it: `AnimationGameplayEventsSingleton` holds, for one
  frame, the gameplay events the blueprints raised, each with its `Entity`; an animation that is
  over, or stopped, raises two for its entity. The text keys of a playing animation come the same
  way in `AnimationTextKeyEventsSingleton` (`Event.ID`, `Event.Time`).
- **Root motion** is written into the entity's `Transform`, and while a blueprint plays the
  visual follows `Transform`: move an animated figure by writing `Transform` and the visual both.
- An animation named `..._SCENE_...` is authored in its cinematic's space and plays metres away
  from the figure's place.

`Ext.ClientVisual.StopAnimation(entity, [flag])` -> `true` or `false`: the
`EoCGE::ASCancelAnimation` event; `flag` is its one argument.

`Ext.ClientVisual.PlayEffect(effect, position, [rotation], [scale], [levelOf])` -> the effect's
entity, or `nil` and an error line (no level loaded, the effect not found, the function not
found). It calls the game's `ls::EffectsManager::PlayEffect`: a new client entity with `Effect`,
`Visual` and `Transform` is made from the effect resource, put into the scene of the level the
client is in and started.

- `effect`: the effect's **name** (`EffectName` of the resource, `VFX_...`); a resource id finds nothing.
- `position` `{x, y, z}`, `rotation` (a quaternion), `scale` (default 1).
- `levelOf`: an entity of the level; the effect entity then carries its `Level`.

`entity.Effect.Flags` tells how it goes: `Playing` within a few frames, `Stopped` when an effect
that does not loop has played out. The entity stays until it is destroyed:
`Ext.ClientVisual.StopEffect(entity, [how])` -> `true`, or `false` for an entity with no `Effect`,
queues one of the effects manager's own orders: `"Stop"` (the effect plays out), `"ForceStop"`
(`Stopped` at once) or `"Destroy"` (the `Effect` component is gone).

**Not opened:** an effect bound to a figure's bones, and the game's own hiding of a figure. A
figure is hidden by destroying it, or by a scale near zero written to `Transform` and
`Visual:SetWorldScale`.

## Injected input

For a mod or a tool that drives the game's own interface - hover, tooltips, clicks - without
moving the operating system's pointer or writing the game's memory. Client only, no developer
mode.

`Ext.Input.InjectMouseMove(x, y)`, `InjectMouseButton(button, pressed, [x, y])` (1 left, 2 middle,
3 right; with a position the pointer moves there first) and `InjectMouseWheel(x, y)` put an SDL
event into the queue the game polls, with positions in window pixels. The game's window manager
takes a pointer's position only from an event that names its own window, so injected motion,
button and wheel events carry the id of the game's window: the game's own pointer follows them.

```lua
Ext.Input.InjectMouseMove(668, 910)          -- the game's own tooltip of that hotbar slot opens
Ext.Input.InjectMouseButton(1, true)         -- a later frame: pressed where the game's pointer is
Ext.Input.InjectMouseButton(1, false)        -- a later frame again: a click
Ext.Input.InjectMouseMove(-1, -1)            -- the pointer leaves
```

A press and its release in one frame are not a click to the game; a real motion of the mouse
inside the window overwrites the position.

**The Lua events of injected input.** `Ext.Events.KeyInput`, `MouseButtonInput` and
`MouseWheelInput` are raised for an injected event too (`InjectKeyPress`, `InjectKeyDown`, ...
and the mouse calls above), in the same place and with the same right to prevent it
(`e:PreventAction()` keeps it from the game). The event carries `Injected = true` (`false` for a
device's). An event the extender's own windows take (a focused IMGUI text field, a click on an
IMGUI window) raises none, as with a real one.

```lua
Ext.Events.KeyInput:Subscribe(function (e)
    if e.Injected and e.Key == "ESCAPE" and e.Event == "KeyDown" then e:PreventAction() end
end)
```

A listener that injects in answer to an injected event cannot hold the game: what is injected
while such an event is being raised waits for the next round of polling (one frame later).

**Not verified:** clicks injected into the game's own interface while its window is in the
background.

## Values of the interface

For a mod that reads or sets a tick box, a toggle or a duration of the game's interface through
`Ext.UI`. Client only, no developer mode. Reading and writing a property of a UI object handles
two value types of the UI library that it did not:

- `Nullable<Bool>` - `ToggleButton.IsChecked` and everything derived from it (`CheckBox`,
  `ls.LSToggleButton`): read as `nil` (no value) or a boolean, written with `nil` or a boolean.
  A colour swatch of character creation (`swatchSkinColourButton`) opens its panel on
  `IsChecked = true` and closes it on `false`; a tick box of the options window flips, and its
  view model's `Value` follows a frame later.
- `TimeSpan` (`ls.LSInputBinding.TapTime`, the durations of `ls.Concertina`): seconds as a number,
  both ways. As with every dependency property, one that was never set reads `nil`, not its
  default, and a value once written cannot be taken back to "not set" from Lua.

Still not read (an error line and `nil`): `ls.LocaString` held as a dependency property (it is
read as a plain property), `Transform2` (`ls.LSWorldMap.ProjectionMatrix`).

## Screenshots, the frame and the window

For a tool that photographs the game, shows its frames elsewhere, or runs it without taking the
keyboard. Client only, no developer mode.

- **`Ext.ClientRender.Screenshot(path, [withOverlay])`** -> `true`, or `false` when a screenshot
  is already on its way, the renderer is not up, or the backend cannot read its frames (DX11
  only). The next frame is written as a PNG below the Script Extender folder of the player's
  profile (where `Ext.IO.SaveFile` writes), a few frames later. Without `withOverlay` the picture
  is the game alone, copied out of the swap chain before the extender draws anything of its own;
  with `true` it is copied after the extender's layer (IMGUI windows, a picture shown through
  them): what the screen shows. The window need not be in front or visible.
  `Ext.ClientRender.ScreenshotPending()` says whether one is on its way.
- **`Ext.ClientRender.MirrorEnable`**, `MirrorInfo`: the finished frame copied on the graphics
  card into a shared texture, which lets a window elsewhere show the game at the game's own rate.
  The cost is one GPU-to-GPU copy a frame, measured at 149.2 against 149.9 frames a second with
  it off.
- **`Ext.ClientRender.SetFrameCap`**, `FrameCap`, `SetLostFocusFrameCap`: the frame caps the
  engine already has (`BaseApp` keeps the caps, `ls::GameClock` the microseconds), written as the
  engine's own setter writes them. Nothing hooked or patched; it refuses to write if the layout it
  checks for has moved.
- **`Ext.ClientRender.KeepInBackground`**, `KeepOffScreen`, `ForegroundLog` (and the settings
  `KeepWindowInBackground`, `KeepWindowOffScreen`): a game started by a tool does not throw its
  window over the person's work. Attempts to bring the window forward are refused, and the window
  is given `WS_EX_NOACTIVATE`, because Windows activates a window with nobody calling anything:
  the first top-level window of a process when it is shown, and the next window in line when the
  one that had the keyboard goes away. Every attempt is recorded with the module and offset of
  whoever made it, refused or not; this game was never seen to ask. Off by default.

## A picture from the disk

`Ext.IMGUI.LoadImageFile(path)` -> `{ width, height }` or `nil`; `Ext.IMGUI.UnloadImageFile(path)`.
Client only, no developer mode. **A convenience of the extender, not coverage of the game**: the
extender reads a png, jpg, tga or bmp file from any path of the machine's file system itself and
makes a texture of it for its own drawing layer. After it the picture is known under that same
path to `AddImage`, `SetBackgroundImage`, a canvas' `AddImage` and `GetImageSize`, until it is
unloaded. Without the call a path with a drive letter or `..` is refused (`AddImage` reads through
the game's data only, the rule `Ext.IO` keeps for every file). It hands Lua nothing of the file
but its size.

```lua
local size = Ext.IMGUI.LoadImageFile("C:/pictures/logo.png")      -- { 512, 512 }
window:AddImage("C:/pictures/logo.png", size)
```

**Limits.** No `.dds`; no images on the Vulkan backend. The game's own interface takes textures
from its texture provider (`ui::TextureProvider`) by name, and none of its kinds reads a picture
by a path of the disk.

## Fixed

- **`Ext.System.AnimationBlueprint:QueueGameplayEventToGenome` (and `...FromGenome`) with set
  arguments closed the game** - upstream's bug, in the release of a genome variant:
  `GenomeVariant::Release` handed the type's destroy function the address of the variant's value
  slot for every type, while a value larger than a pointer (a set is three) lives behind that
  slot. The value itself is now handed over for a large type; the call that closed the game passes
  fifty times over. What the blueprint does with such an event was not examined;
  `Ext.ClientVisual.PlayAnimation` is the way that is known to play.

## Settings this fork adds

In `ScriptExtenderSettings.json`, beside upstream's:

| Setting | Default | What it does |
|--|--|--|
| `KeepWindowInBackground` | false | Refuses the game's attempts to bring its window forward |
| `KeepWindowOffScreen` | false | Puts the window where no screen reaches when it is created |
| `ShiftOpensConsole` | true | Holding Shift at start opens the console |
| `StartupVideos` | true | Off, the game starts with no startup video, whatever the mods declare |
| `ModHints` | true | Off, the loading screen's hint list stays as the game's own file gives it, whatever the mods ship |
| `EarlyModInterface` | true | Off, the mods' interface is merged only at the game's own time (the end of `LoadModule`), and the first loading screen is the game's alone |
| `DirectSaveLoad` | false | On, a save the command line asks for (`-loadSaveGame`, `-continueGame`) is loaded without the main menu: the menu's states the game queued at its start are taken out once its `Init` has queued the save's load. A combination the game does not make by itself: nothing of the menu runs before the first session |
| `WideCommandLine` | true | Off, the game reads its command-line values in the system's ANSI code page only (a non-ASCII save name is not found) |
| `CrashDumpDirectory` | empty | Folder for crash dumps, `<pid>-<yyyymmdd>-<hhmmss>.dmp` and a `.txt` with the faulting `module+0xRVA`; when set, crash reporting is on even with `SendCrashReports` off |
| `CoverageAtStart` | empty | Developer mode: a file of the executable's function starts; coverage counters from the extender's start, a record at every state the client enters up to the main menu ([Coverage](#coverage)) |
| `LaunchCrashReporter` | true | Starts the reporter window after a dump |
| `FastLoadClientFrames` | false | On, the client's frame caps are lifted from its `StartLoading` until it is in `Running`/`Menu`, then put back (a load's states advance once per frame) |
| `FastLoadServerTick` | false | On, the server worker's tick period (+0x90, 33333 us) is set to 0 while the server is in a loading state and put back at `Running` |
| `FastLoadStreamingWait` | false | On, `PrepareRunning` ends at its first update: its `ls::StreamingPoller` (0.5 s least, 20 s most) is set to 0; textures go on streaming in `Running` |
| `FastLoadFades` | false | On, `GlobalSwitches.FadeSpeed` (0.5 s) is set to 0: the state machine's screen fades around a load are instant (so are the game's other fades that read it) |
| `FastLoadLuaGC` | false | On, the game's own embedded Lua 5.3 (the AI behaviour scripts of `ls::anubis::System`) has its collector held on the server's thread while the server is in a loading state: its step (`unplaced::luaC_step`) sets the debt to -24000 bytes instead of collecting, as Lua does for a stopped collector; it collects as the game's once the server leaves the loading states (Basic_Level_A: the server's LoadSession 2.95 -> 2.30 s) |
| `RenderCut` | false | On, the main thread's frame (`ls::RenderThread::Update`) is wrapped: with `Ext.StartupVideos.RenderCut(1)` (or `RenderCutSkip`) the render frames of `ls::RenderFrameSystem` are turned off before each update (enabled byte +0x42F0), so nothing is recorded, drawn or presented while the update does the rest (its waits for the client, window messages, dispatched tasks, resources, render commands); `RenderCut(0)` draws again, `RenderCut(N)` draws one update in N. `RenderCut()` reads the counters (frames, the client's and server's ticks, each thread's CPU, memory). A picture (`shot`) needs drawing on |
| `RenderCutSkip` | false | With `RenderCut`, nothing is drawn from the start |
| `FastLoadStreamingGate` | false | With `FastLoadStreamingWait`, only `PrepareRunning`'s least wait is cut and its most wait kept: the state still waits for the root level's start dialogue to be ready (`ecl::GameStatePrepareRunning::Update`'s query of `eoc::ServerRootLevelStartDialogComponent` without `ecl::ClientRootLevelStartDialogReadyComponent`), which a most wait of 0 passes at once - the story's arrival then lands after `LevelGameplayStarted` (W3 round 5) |
| `RenderCutSkipPaceUs` | 0 | With `RenderCut`, a skipped update (nothing drawn) is paced: the main thread sleeps on a high-resolution waitable timer until this many microseconds after the update began, instead of looping straight into the next one; a drawn update is never paced. Measured (W3 round 7, a start of Basic_Level_A with nothing drawn from the process start): 33,896 skipped updates by Running unpaced, 2,530 at 4000 us; the process's CPU by Running 71.2 -> 36.7 s, Running 18.7 -> 17.3 s. `Ext.StartupVideos.RenderCut()` reads `SkipPaceUs`, `PacedSleeps`, `PacedSleptMs` |
| `RenderCutSkipPaceStartOnly` | false | Ends pacing at the client's first `Running` when `RenderCutSkipPaceUs` is set. The timeline records `RenderCut pace off` and in-session loads keep their unpaced timing. |
| `ExitWitness` | false | On `ExitProcess` or self-termination through `TerminateProcess`, writes `exit-<pid>.txt` in `LogDirectory` with the exit code, callers, recent exceptions and startup timeline events. This includes exits through the game's unhandled-exception filter. Fail-fast and external termination bypass those calls. |
| `LargeAllocTraceMB` | 0 | Measuring only: kernel32's `VirtualAlloc` wrapped from the extender's start; a call of at least this many MB is written into the start's timeline as `Step` rows - the size, type, protection and address, then up to 8 return addresses as `module+0xRVA` (the first 64 such calls). W3 round 7 |
| `RenderCutFromRunning` | false | With `RenderCut`, nothing is drawn from the client's first `Running` on: the start is drawn. With nothing drawn from the process start the interface's view model of the host (`gui::VMCharacter`) never gets `IsPlayableCharacter` nor its player, `gui::VMPlayerCharacterProperties::SetActiveDialogue` is never reached, and a dialogue's window shows nothing (the server's dialogue waits on it): W3 round 5 |
| `TreasurePin` | false | On, `esv::inventory::TreasureGenerationSystem::GenerateTreasure` (a provisional pattern, bg3_dx11 only) gets a wrapper, inert until `Ext.Debug.PinTreasure(seed)`: then each treasure table rolled first seeds the server's saved random (`esv::EocServer` +0xB8, the one `ls::Random` every treasure table draws from, seeded from a millisecond clock at the server's start) from the seed and the entity's UUID, so an entity's loot no longer depends on the order a level's setup makes its treasure requests in. A cheat for differential checks; `Ext.Debug.PinTreasure()` takes it off. W3 round 6 |
| `IdleSleep` | false | On, the game threads' tick wait (`unplaced::TickWait`: the server's worker, the client's game thread), which spins out the rest of a tick (QueryPerformanceCounter, SwitchToThread, queued worker jobs, a tight spin for the last millisecond), first sleeps on a high-resolution waitable timer until 1.5 ms before the wait's end when the wait is longer than 3 ms; the game's own wait does the rest and keeps its carry, so ticks end when the game's would. An idle game kept open stops burning a core per thread. `Ext.StartupVideos.Headless()` reads the counters |
| `StubFileExtensions` | "" | Measuring a headless game: a comma-separated list of extensions (`.dds,.wem`); the engine's opens (`ls::FileReader`) of a path ending in one are given a path that does not exist, so the game takes its own fallback for a missing file. Counted by `Ext.StartupVideos.Headless()`. Measured (W3 round 4): `.bshd` (shaders) missing - the process ends at the start; `.dds` (textures) missing - it ends in SwapLevel: the game has no fallback for either |
| `StubFileTarget` | "" | With `StubFileExtensions`: the path a stubbed open gets instead of a missing one - one small valid file of the class (a 64x64 engine texture for `.dds`), so the game gets a valid object |
| `LoadTrace`, `LoadTraceSampleMs` | false, 5 | Measuring: every file opened (`ls::FileReader`) and read (`ReadFile`), memory/IO/thread CPU at every game state, a sampling profiler of the game's threads; written out by `Ext.StartupVideos.LoadTraceDump(path, sinceMs)` (Extender/Shared/LoadTrace.h). Costs ~150 MB and a little CPU |

## Upstreaming, and our own build

These are improvements to Norbyte's work, and belong back in it where he wants them. Whether he
takes a change is his decision, so the fork is kept ready for both ways at once:

- **Upstream**: each addition is offered as its own pull request against `Norbyte/bg3se`, one
  subject at a time, with the evidence for each change. An addition is therefore one separable
  unit: its own files where it can (`Extender/Client/StartupVideos.*`, `ModHints.*`,
  `EarlyModUI.*`, `GameModules.*`, `Extender/Shared/StartupTimeline.*`, `Lua/Libs/DebugCover.inl`,
  ...), its own player's switch in the settings below (off gives the game as it is), its own
  section on this page and in [Docs/API.md](Docs/API.md), and its own commits. Taking one needs
  none of the others, except where its section says so (the early readers share
  `GameModules`).
- **Our own build**: the same additions, all together, released from `gatekit-v34` as a build of
  its own named **BG3GKSE** (the release, its archive and the DLL's product name; the file stays
  `BG3ScriptExtender.dll`, the name upstream's loader loads, so it replaces upstream's one for one).
  The product name in [BG3Extender.rc](BG3Extender/BG3Extender.rc) and the release workflow
  (`.github/workflows/bg3gkse.yml`, "Building") are ours
  alone and are never offered upstream (GateKit's `se-release.json`; `gatekit se install` fetches it). It does not wait for
  upstream. An addition upstream takes becomes upstream's at the next rebase, and the difference
  shrinks.
- **Nothing here needs GateKit to be built.** The mappings of the game's functions that GateKit's
  registry writes stand in [BinaryMappings.xml](BG3Extender/GameHooks/BinaryMappings.xml) as
  ordinary lines, each after a comment `<!-- code:<id>; places -->` naming the registry's id and
  the function's place in each executable: a pattern of the function's start found once in every
  executable. `gatekit registry mappings sync <BinaryMappings.xml> --check` says whether they are
  still as the registry has them (a check before a build), and without `--check` writes them
  again after the registry moved to a new build of the game. A mapping whose pattern is not at a
  function's start, or that finds a global through an instruction (`@ref`), is written by hand and
  has no such comment.

A player installs one DLL either way: this fork's `BG3ScriptExtender.dll` in place of
upstream's, through the same loader. GateKit's own mod is for driving and testing the game and is
never needed to play; a mod that uses an addition here is an ordinary mod, and on upstream's
extender its files are simply not read.

## Building

As upstream: Visual Studio 2026 with the C++ toolchain (v145), `BG3Tools.sln`, the `Game Release`
configuration. GateKit users do not build it - `gatekit se install` fetches a release. A build
folder holds the DLL, its PDB and, for GateKit, `RegistryMappings.xml` (above).

The build needs nothing beyond the externals `pull-externals.bat` fetches and builds (run once,
from an x64 Native Tools prompt): `stb_image.h`'s folder is among the project's include paths, and
the Abseil and utf8_range libraries libprotobuf-lite links against are taken from protobuf's own
build by folder. From the repository's root, with no `CL` or `LINK` in the environment:

```
msbuild BG3Tools.sln -t:BG3Extender -p:Configuration="Game Release" -p:Platform=x64 -m
```

gives `x64\Game Release\BG3ScriptExtender.dll` and its PDB. The post-build step copies the DLL into
`GameBinDir` (`-p:GameBinDir=<the game's bin>`; upstream's own path by default) and is skipped
where that folder does not exist.

**Releases.** [.github/workflows/bg3gkse.yml](.github/workflows/bg3gkse.yml) builds the same on a
GitHub Windows runner (`windows-2025`, Visual Studio 2026), with the externals kept in its cache
until `pull-externals.bat` changes. Run by hand it keeps the build as the run's artifact and
publishes nothing. A pushed tag `BG3GKSE-<major>.<n>` - `<major>` is `RES_DLL_MAJOR_VERSION` in
[resource.h](BG3Extender/resource.h), `<n>` our own count - makes a release of that name holding
`BG3ScriptExtender.dll`, `BG3ScriptExtender.pdb`, `SHA256SUMS.txt` and `BG3GKSE-<major>.<n>.zip`
(the DLL and the PDB). The run refuses a DLL whose product name is not BG3GKSE. The workflow is
ours alone, like the product name, and is never offered upstream.

Before a tag is pushed, on a machine with the game and GateKit's symbol database:

```
gatekit registry mappings sync BG3Extender/GameHooks/BinaryMappings.xml --check
```

It cannot run on the runner: it finds each marked mapping's pattern again in the game's
executables through the symbol database, and neither the game nor that database (built from the
game's files) may be put on a public machine. `RegistryMappings.xml` is not in the release for the
same reason (`registry mappings write` scans the executables); `gatekit se install <folder>`
copies it from a build folder that has one.

GateKit takes a release by its pin: `se-release.json` in GateKit names `bg3gk/bg3se`, the tag, the
zip and the zip's SHA-256 from `SHA256SUMS.txt`; `gatekit se install` with no folder fetches that
zip and checks the hash.

The builds, newest first; each is normal mode, `Game Release` x64:

**`v34-gatekit`**, from `4d1f8147`, the head of `gatekit-native`: everything on this page, with
the registry's table of patterns beside the DLL. DLL SHA-256
`e324d034275ab61be8cb9bbd1ca0413e14385ed7a97e0649f2587071ca2834da`.

**`v34-adjust`**, from `db75b353` on `gatekit-native`: the roll events with what a roll is for and
`RollAdjusted`, the addresses, the function probes and the coverage counters. DLL SHA-256
`15b6b9e6a4acb35a6de01df23b73c0af09210768f11d2281b6d09b3890a1ba2a`.

**`v34-ui`**, from `a87a1666` on `gatekit-native` with the interface and input changes: injected
pointer events that name the game's window, the Lua events of injected input, `Nullable<Bool>`
and `TimeSpan` values of the interface, `Ext.ClientRender.Screenshot(path, true)`,
`Ext.IMGUI.LoadImageFile` and the genome variant fix. DLL SHA-256
`3bd91c1f76d8921cc089d4dfc8c6a9ab9553b8c0c482a6f55ea1ea1bc3204c33`.

**`v34-anim`**, from `1526c135` on `gatekit-native` with the animation and effect changes:
`Ext.ClientVisual.PlayAnimation`, `StopAnimation`, `PlayEffect` and `StopEffect`. DLL SHA-256
`ff6148b53663d92b73b0ad471effad3db6a014d8ce42c92e75c4406e3035dac3`.

**`v34-figure`**, from `9a59c008` on `gatekit-native` with the figure's changes:
`Ext.ClientVisual.CreateDummy` and `DestroyDummy`. DLL SHA-256
`6fb700924b5794065aa906e0af51a59a5949e34a0705e9ff122726ac53557cdf`.

**`v34-final`**, from `5655196a` on `gatekit-native`: `Ext.Stats.GetSpellRollChance`. DLL SHA-256
`e199ce7c32d22ca1e7a5f58dc358848eb2ed5b26ee11a74ad5e38f9e50493245`.

**`v34-instruments`**, from `7b9b3ad1` on `gatekit-v34-instruments`: the memory instrument made
sound and completed. DLL SHA-256
`812f1c948d5a2e62049840bfc38d6e3281c5c2110648d88a75f13259fae1be78`.

**`v34-tools`** (whole-program optimisation), from `fd2239ff`: `gatekit-v34` with the memory
instrument, the ECS tracer's stamps and `GlobalSwitches::SoundSettings` mapped. DLL SHA-256
`2b983653949938cdcb0044e157dc41dd1e03d2abd0457f948900343cc1eb95bf`.
