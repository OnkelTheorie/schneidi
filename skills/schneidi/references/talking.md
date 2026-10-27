# Interview, podcast, vlog, talking head

Goal: same content, no dead air, no slips - but still natural, not robotic.

## Look first
- `probe`: several audio streams? Recorders like OBS keep e.g. "System" and "Mikrofon" apart. Use the voice stream
  for analysis (`audio_stream` on media files, `tracks` on projects).
- `transcribe` with `language` set (and `words` true if you want to cut words). Read it: what is the content,
  where are slips, repetitions, false starts?

## Remove pauses
- **From a media file** (new project; `template` "podcast" names the tracks Voice 1, Voice 2, Music, so `check` and
  `duck` know them): `silence` on the file with `project` set → the `sound` ranges go straight into `keep` (jump cut).
- **On an existing timeline** (also live in the app): `silence` on the project → its `silence` ranges are timeline
  ranges → `delete_range` with `ranges` (ripple) removes them all in one op. Don't cut piece by piece - every cut
  moves the later times; `delete_range` applies all ranges together.
- Defaults that sound natural: `min` 0.5-0.7 s (shorter pauses are breathing), `pad` 0.1-0.15 s. A fast vlog may go
  tighter (`min` 0.3 s); interviews and podcasts stay looser so answers can breathe.
- If the result removes much more than expected (e.g. half of the length), the threshold may be catching quiet
  speech: try `threshold_db` -40 or -45 and compare `kept_s`.

## Remove slips, repetitions, filler
- With `words` from `transcribe`: collect the time ranges of the unwanted words/phrases and remove them with one
  `delete_range` (`ranges`). Times from a media file are source times - map them onto the timeline or transcribe the
  project instead (its times are timeline times).
- Whisper often leaves out "uh/um/äh"; don't promise to remove filler you cannot see in the transcript.
- For a false start ("We went - we drove to …") cut the first try, keep the clean one.

## Sound
Speech is the product: see [audio.md](audio.md) - `denoise` for noisy rooms, EQ high pass ~80 Hz, `normalize`
(lufs) so all parts sound equally loud, about -14 LUFS for the web, -16 for podcasts is also common.

## Check
New length vs. old (`kept_s` / `info`), `silence` on the result (no long pauses left), a `frames` sheet around a few
cuts if there is video (jump cuts are fine for talking heads, but not mid-word). Subtitles: [subtitles.md](subtitles.md).
