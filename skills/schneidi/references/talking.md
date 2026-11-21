# Interview, podcast, vlog, talking head

Goal: same content, no dead air, no slips - but still natural, not robotic.

A vertical podcast clip (speaker, topic on top, word captions, logo): the `podcast-clip` template
([templates.md](templates.md)) - fill it with the clip's range, then transcribe and `captions`.

## Look first
- `probe`: several audio streams? Recorders like OBS keep e.g. "System" and "Mikrofon" apart. Use the voice stream
  for analysis (`audio_stream` on media files, `tracks` on projects; `transcribe` already picks voice-named ones).
- `transcribe` with `language` set (and `words` true if you want to cut words). Read it: what is the content,
  where are slips, repetitions, false starts?

## Remove pauses
- **Conversation on a timeline (two or more microphones, also live in the app):** `{"op":"cut_pauses"}` cuts only
  where EVERY voice track is quiet (`tracks` "A1,A2"; default the voice tracks), each track measured against its own
  noise floor, so one hissing microphone does not hold up the cuts and nobody's answer is cut away. `min_pause`
  "0.6s", `pad` "0.15s" (quiet kept at both ends). All tracks ripple together (music, picture, subtitles; markers in a
  pause land on the cut); a locked track with a clip in a pause = `TRACK_LOCKED`. The notes list every range (`kind`
  pause|filler, `tracks`, `length_s`): to let the user choose, run it with `dry_run` (or `plan`), then
  `{"op":"cut_pauses","ranges":[[from,to],…]}` with the ranges to cut. `fillers:true` also cuts hesitation sounds
  (äh, ähm, uh, um, euh, eee …) while the others are quiet - only those the transcript has: transcribe with
  `verbatim:true` first, whisper leaves most of them out otherwise.
- **Let the user pick the pauses** (also on the app's Direct page): `direct` `recipe` `talk` with `project` and
  `timeline` (default `@current`; it works on a timeline, not on files) suggests each pause and filler (`kind`
  pause|filler, `track`, a reason with the pause's whole length - `cut` is the part cut out, the pads stay - or the
  word); `options` `min_pause_s`, `pad_s`, `fillers`, `tracks`, `language`, `target_lufs` (sets the master fader).
  `fillers` on recordings without a transcript: whisper writes one (verbatim) while suggesting, the apply keeps it in
  the project (a `transcribe` of the project only answers, it keeps nothing). A pause at the timeline's end is cut
  to the end (no pad left behind). `apply` cuts the chosen ones out of a copy - a new timeline next to the
  original, which stays as it was; the answer's `timeline` names it. The timeline changed after suggesting =
  `SESSION_STALE`. Room tone is not added: offer `room_tone` (below).
- **From a media file** (new project; `template` "podcast" names the tracks Voice 1, Voice 2, Music, so `check` and
  `duck` know them): `silence` on the file with `project` set → the `sound` ranges go straight into `keep` (jump cut).
- **On an existing timeline** (also live in the app): `silence` on the project → its `silence` ranges are timeline
  ranges → `delete_range` with `ranges` (ripple) removes them all in one op. Don't cut piece by piece - every cut
  moves the later times; `delete_range` applies all ranges together.
- Defaults that sound natural: `min` "0.5s"-"0.7s" (shorter pauses are breathing), `pad` "0.1s"-"0.15s" (times:
  a plain number counts frames). A fast vlog may go tighter (`min` "0.3s"); interviews and podcasts stay looser so
  answers can breathe; "only the long pauses" = `min` "0.8s"-"1s".
- If the result removes much more than expected (e.g. half of the length), the threshold may be catching quiet
  speech: try `threshold_db` -40 or -45 and compare `kept_s`.

## Remove slips, repetitions, filler
- With `words` from `transcribe`: collect the time ranges of the unwanted words/phrases and remove them with one
  `delete_range` (`ranges`). Times from a media file are source times - map them onto the timeline or transcribe the
  project instead (its times are timeline times).
- Whisper often leaves out "uh/um/äh": `transcribe` with `verbatim:true` writes more of them (a starting text full of
  them); still, don't promise to remove filler you cannot see in the transcript. `cut_pauses` `fillers:true` cuts the
  ones it finds.
- For a false start ("We went - we drove to …") cut the first try, keep the clean one. Sentences said again right
  after themselves: `{"op":"best_takes"}` keeps the last good take of each (from the clips' transcripts).
- **From a script:** `{"op":"rough_cut","script":"…"}` finds each sentence in the transcripts of the project's files
  (forgiving misheard words), takes the last complete take (`takes` `"best"` = the highest rated) and lays them out in
  the script's order in a new timeline; the notes list each sentence's take, the alternatives and `missing` sentences.
  Best as a plan (`plan` true) when the user wants to choose. Or `direct` `recipe` `script` with `files` and/or
  `project` (`options` `script` or `script_file`; `files`, else the project's media; it transcribes what has no
  transcript yet): one suggestion per take (`kind` take, `sentence`), the chosen take of each sentence on ("best take
  of sentence N"), the alternatives off ("another take of sentence N") - switch them, then `apply` lays the chosen
  takes in the script's order on a new timeline (a take switched on goes to its own sentence's place). Numbers match
  however they are written ("Neuner" = "9er", "einundzwanzig" = "21").

## Smoother cuts (offer them, they are not automatic)
- **Room tone:** after a pause cut the gaps between clips of the voice track are dead silent (with a gap left by
  `delete_range` ripple:false, or a pause you kept). `{"op":"room_tone","track":"A1"}` fills every gap with a loop of
  the recording's own quiet, at its noise floor, crossfaded so nothing clicks; `from`/`to` limit it. The notes say
  which part of the file it used.
- **J/L cuts** for interviews and dialogue: the sound changes a moment before or after the picture, which hides the
  cut. `{"op":"jl_cut","at":"0:12","audio":"-0.4s"}` = J (the next person is heard before we see them), `"+0.4s"` = L
  (the previous sentence runs on under the next shot); 0 puts it back. Only the sound edge moves, picture and sync
  stay. 0.2-0.6 s sounds natural; not enough material in the file = an error that says how far it can go.
- **Jump cuts** after a pause or word cut (one take, a part skipped; `check` lists them as `jump_cut`):
  `{"op":"hide_jumps"}` pushes every second clip in (zoom 1.12 around the speaker), so the jump looks like a second
  camera.
- **B-roll to what is said:** after `index` (shots described with `annotate`), `{"op":"broll"}` puts the matching shot
  of the material over each sentence on V2 (picture only); best as a plan.

## Sound
Speech is the product: see [audio.md](audio.md) - `clean_voice` on the voice track (noise, pauses, even level; `denoise` for light hiss only), EQ high pass ~80 Hz, `normalize`
(lufs) so all parts sound equally loud, about -14 LUFS for the web, -16 for podcasts is also common.

## Check
New length vs. old (`kept_s` / `info`), `silence` on the result (no long pauses left), a `frames` sheet around a few
cuts if there is video (jump cuts are fine for talking heads, but not mid-word). Subtitles: [subtitles.md](subtitles.md).
