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

Every change of meaning — not only of layout — is verified in the running game before it lands.
What is verified and what is merely deduced is recorded with the change.

## Upstreaming

These are improvements to Norbyte's work, and belong back in it where he wants them. Nothing
here has been offered upstream yet; when it is, it will be as ordinary pull requests against
`Norbyte/bg3se`, one subject at a time, with the evidence for each change.

## Building

As upstream: Visual Studio with the C++ toolchain, `BG3Tools.sln`, the `Game Release`
configuration. GateKit users do not build it — `gatekit se install` fetches a release.
