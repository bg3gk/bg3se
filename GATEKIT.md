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

Every change of meaning — not only of layout — is verified in the running game before it lands.
What is verified and what is merely deduced is recorded with the change.

## Upstreaming

These are improvements to Norbyte's work, and belong back in it where he wants them. Nothing
here has been offered upstream yet; when it is, it will be as ordinary pull requests against
`Norbyte/bg3se`, one subject at a time, with the evidence for each change.

## Building

As upstream: Visual Studio with the C++ toolchain, `BG3Tools.sln`, the `Game Release`
configuration. GateKit users do not build it — `gatekit se install` fetches a release.
