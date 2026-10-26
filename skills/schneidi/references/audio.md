# Sound: loudness, music under speech, cleanup

## Targets
- Web / streaming (YouTube, TikTok, Instagram, Spotify): about **-14 LUFS** integrated, true peak ≤ **-1 dBTP**.
- Podcasts often -16 LUFS; broadcast (EBU R128) -23 LUFS.
`loudness` measures a file, a project's mix (as exported) or single clips.

## Order of work
1. **Even out the clips**: `normalize` (`mode` `"lufs"`) on the speech clips so every part sounds equally loud.
2. **Clean the voice** if needed: `denoise` effect on the audio clips (noisy rooms, fans), `mixer` EQ on the voice
   track (band 1 high pass ~80 Hz against rumble), `mixer` `compressor` for uneven speakers or shouting in games.
3. **Music under speech**: music on its own track, then `duck` (music = `track`/`clips`, `under` = the voice
   track) so it dips while someone talks; a music bed sits ~15-20 dB below the voice.
4. **Master**: `mixer` on `"master"` with `limiter` true first, then `target_lufs` -14 - it measures the mix and
   sets the fader. The limiter keeps the peaks below -1 dBTP.
5. Short `fade`s at the very start and end avoid clicks.

## Recordings with several audio streams
OBS and similar recorders store e.g. "System" and "Mikrofon" separately. `probe` lists the streams; the user's import
presets decide which ones land on the timeline (by default every stream, one track each). `audio_stream` changes what
a clip plays; `tracks` in `silence`, `transcribe` and `loudness` picks what is analysed.

A recording with a **Mix stream plus its single streams** then plays everything twice (too loud, clipping). Look at
`info` after placing such a file: if "Mix" and its parts are all enabled, switch off one side with `enable` (usually
the Mix, so the voice and game stay separately adjustable) and confirm with `loudness`.

## Check
`loudness` on the rendered file: integrated within ±1 LU of the target, true peak ≤ -1 dBTP. If the music drowns
speech, lower it or duck harder (`amount_db` -15 to -20).
