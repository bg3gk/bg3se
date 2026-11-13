### Sound settings: mapped (2026-11-13)

`ls::GlobalSwitches::SoundSettings[12]` is mapped, so
`Ext.Utils.GetGlobalSwitches().SoundSettings` works and a mod can read the player's own sound
settings. The twelve keys, in the array's order, are `MasterVolume` (Wwise RTPC MAIN),
`FxVolume` (SFX), `AmbientVolume`, `UIVolume`, `MusicVolume`, `MusicFightVolume` (Music_Fight),
`CutsceneVolume` (Cinematics), `VoiceVolume` (VO_Master), `VoiceVolumeDialog` (VO_Dialog),
`VoiceVolumeNarrator` (VO_Narrator), `VoiceVolumeOverhead` (VO_Overhead) and `RumbleVolume` —
the last one does have an RTPC after all, `RTPC_Rumble`. The options screen shows seven of them,
and every `DisplayName` resolves to that row's label there.

`SoundSetting::ConfigValue[3]`: `[0]` is the configured value, `[1]` the value in effect (the
only one `UpdateProc` sends to the RTPCs), `[2]` the built-in default. Seen live with the master
at 37 and the music at 42 in the profile: `37 37 100` and `42 42 100`, everything else
`100 100 100`. The evidence, the addresses and the two things that are still unaccounted for
are in GateKit's `research/se-fork/upstream/sound-settings.md`.

Still open: what the options screen writes when a slider moves (it does not draw these floats,
and the settings writer serialises `[2]`, the default, which cannot be the whole story), and
`field_40`, a byte that is 1 on `VoiceVolumeDialog` and 0 on the other eleven.

**This is the first offer to Norbyte**, prepared as the branch `offer/sound-settings` cut from
`base/v34-imgui_v1_93-2ea16d51`. Nothing is pushed; whether and when it is sent is the owner's
decision.
