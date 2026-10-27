# Highlight reel (games, sports, events)

Goal: the best moments, tight, with a rising arc, often with music. For a vertical video (Short, Reel, TikTok)
also read [social.md](social.md).

## Find the moments
- `highlights` with all clips at once (`files`) gives candidates as source ranges with lead-in and lead-out, best
  first (`best`), and a ready `keep` list per file. Pass `project` so the frames match your project.
- **Games with a kill feed**: look at the kill feed first with `frames` + `crop` (e.g. `"0.7,0.0,0.3,0.3"` = top
  right) on a moment with a kill, pick the region and the color that marks the player's *own* kills, then pass
  `region` + `color`. Without that, `highlights` uses the sound alone (bursts of loud moments) - fine for sports,
  events, vlogs. A thin mark needs a higher `tolerance` than a filled one: CS2 frames the player's own kills in red
  (`region` "0.72,0.04,0.28,0.2", `color` "#d8444a", `tolerance` 40). Fewer `kills` than you saw in the kill feed:
  raise `tolerance` step by step (too high counts red walls and blood as kills).
- Look at the top candidates (`frames` with `at` around each kill time, or a cropped sheet) before you trust them.
  Drop near-duplicates and moments that only are loud. Banners in the kill feed's colour (a yellow "BUY PHASE")
  are not counted as kills but listed in `banners`; a `kill_feed_most_pct` far above the other files still means:
  look at that file's kills.
- **Lag**: game capture with `variable_frame_rate` stutters where the game lagged (the picture stands still).
  `highlights` marks such candidates with `stutter` and gives their `clean` parts (with the kills in them) and a
  `keep_clean` list; `probe` with `gaps` lists the jumps of any file. Cut out the lagging part, keep the moment -
  don't drop a multi-kill because of half a second of lag.

## Build
- Project in the material's frame rate (60 fps gameplay → `fps` 60).
- `keep` places several source ranges of one file one after another; use one `keep` per file, or `append`/`place`
  for single ranges in the order you want.
- Rules of thumb:
  - each moment at least ~1.5 s, lead-in at most ~2 s before the action, lead-out ~0.5-1 s after it;
  - open strong (a good moment as the hook), keep the best for the end (rising arc);
  - multi-kills and clutch moments beat single kills; vary the moments, don't repeat the same kind five times;
  - show a multi-kill or clutch in one piece (a `speed_ramp`: fast between the kills, normal on them), don't chop
    it into short bits;
  - 30 s ≈ 5-8 moments. Hit the requested length within about a second.
- Short `transition`s (cross dissolve 0.2-0.3 s) or hard cuts; hard cuts on the beat feel best.
- With music, follow the song, not a fixed grid: the strongest moments on its accents (the drop, the first beat
  after a break), a change of pace where the song changes, the end on the end of a phrase. Same scene length for
  every moment feels mechanical. `beats` tells you where those are (see Music).
- Effects (zoom punch, flash, shake) for the big moments only; the same effect on every kill wears off.
- A sound on every kill (or hit, beat): all copies on **one** track, each starting on its moment (cut off silence at
  the file's start first, `silence` shows it). When a kill comes before the previous sound has ended, end the
  previous copy there (`trim`), like a sampler starting the sound again - copies on separate tracks pile up, get
  loud and clip. Keep them overlapping only when the user asks for it.
- `accent` puts them on many moments in one op, as keyframes: `{"op":"accent","at":["3.3s","7.2s"],"preset":"hit"}`
  (kill times from `highlights`, drops from `beats`). Presets: `punch`, `flash`, `shake`, `hit` (all three, a kill),
  `drop` (big, for the music drop), `pulse` (small, for beats); `strength` scales one, `effects` lists them. Don't
  write punches and flashes as single `keyframe` ops. A `transform` `zoom`/`x`/`y` without `at` afterwards moves
  the accents along (no need to clear and redo them).
- Vertical (9:16) from 16:9 gameplay: one track with `transform` `fit` `"blur"` on every clip (see
  [social.md](social.md)), not a blurred copy of each clip on a second track.
- Optional intro/outro: `title` for 1.5-2 s (name of the player/event), not longer. A title that names a moment
  ("UNREAL", "ACE") appears on that moment, not before.

## Music
- Cut to the song: `beats` gives bars, phrases, breaks and drops in one call. Pick the part on the song file (with
  `project`); once the song is on the timeline, run `beats` on the project with `tracks` = the music track: then
  every time is a timeline frame, no offset to add. Beat k is at `beat_phase` +
  k * `beat_period`, bar ones every `downbeats.every_s`; `phrases` are the sections (4 or 8 bars) with their
  energy, `drops` the big moments (best first). Put the best moment on the top drop, change scenes on bar ones or
  phrase starts, let the end land on a phrase end. `apply` `"markers"` marks the phrase starts on the timeline.
- Put the music on its own audio track (`place` with `track` `"A2"` at 0, or a new track), cut it to the video
  length, `fade` out the last 1-2 s.
- Keep the game/event sound - it carries the action: shots and kills must stay audible. Measure first: `loudness`
  of the song and of the game clips (a mastered song is often -4 to -8 LUFS, game sound -15 to -20). Then lower the
  music until it sits a few LU under the game sound at the moments, or let the sound lower it (`duck` with the
  music as `track`/`clips` and the game track as `under`). Never only turn the game sound down. `check` warns
  (`under_music`) when the game sound ends up more than 15 LU under the music.
- A title on the moment the song sings or shouts it: `transcribe` the song with `music` true and `words` true (only
  the part you use, with `from`/`to` on a project, it is slow); whisper may hear it a bit wrong.
- See [audio.md](audio.md) for loudness: master limiter on, about -14 LUFS.

## Check
`frames` sheet of the timeline (one frame per moment) - every moment shows action, no menus or dead time. Then
render and follow [check.md](check.md).
