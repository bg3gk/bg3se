
### Sound settings are declared but not reachable from Lua (found 2026-11-13)

`ls::GlobalSwitches::SoundSettings[12]` is declared in `GameDefinitions/Misc.h` but is absent
from the property map, so Lua gets "property does not exist" for it. The twelve keys were
recovered from the game executable's own strings and checked live: `MasterVolume` (Wwise RTPC
MAIN), `FxVolume` (SFX), `AmbientVolume`, `MusicVolume`, `MusicFightVolume` (Music_Fight),
`CutsceneVolume` (Cinematics), `UIVolume`, `VoiceVolume` (VO_Master), `VoiceVolumeDialog`
(VO_Dialog), `VoiceVolumeNarrator` (VO_Narrator), `VoiceVolumeOverhead` (VO_Overhead) and
`RumbleVolume` (no RTPC; controller vibration). The options screen shows seven of them.

What is unknown: what the three floats of `SoundSetting::ConfigValue` mean. The one function
that mentions `RTPC_Volume_MAIN` is a 196-byte FixedString initialiser and says nothing;
answering it needs the decompiler on the code that reads the array.

GateKit does not depend on this — `Ext.Audio.SetRTPC` sets the same value a slider ends up
setting, which is what `GK.Sound` uses. What the mapping would add is *reading the player's own
settings*, which nothing can do today.

**This is a good candidate for the first pull request to Norbyte**: small, self-contained,
obviously correct, and useful to anyone. Offer it before arriving with a large branch — but only
on the owner's word, like every other proposal upstream.
