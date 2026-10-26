# Highlight reel (games, sports, events)

Goal: the best moments, tight, with a rising arc, often with music. For a vertical video (Short, Reel, TikTok)
also read [social.md](social.md).

## Find the moments
- `highlights` with all clips at once (`files`) gives candidates as source ranges with lead-in and lead-out, best
  first (`best`), and a ready `keep` list per file. Pass `project` so the frames match your project.
- **Games with a kill feed**: look at the kill feed first with `frames` + `crop` (e.g. `"0.7,0.0,0.3,0.3"` = top
  right) on a moment with a kill, pick the region and the color that marks the player's *own* kills, then pass
  `region` + `color`. Without that, `highlights` uses the sound alone (bursts of loud moments) - fine for sports,
  events, vlogs.
- Look at the top candidates (`frames` with `at` around each kill time, or a cropped sheet) before you trust them.
  Drop near-duplicates and moments that only are loud.

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
  every moment feels mechanical.
- Effects (zoom punch, flash, shake) for the big moments only; the same effect on every kill wears off.
- Optional intro/outro: `title` for 1.5-2 s (name of the player/event), not longer. A title that names a moment
  ("UNREAL", "ACE") appears on that moment, not before.

## Music
- Put the music on its own audio track (`place` with `track` `"A2"` at 0, or a new track), cut it to the video
  length, `fade` out the last 1-2 s.
- Keep the game/event sound - it carries the action: shots and kills must stay audible. Measure first: `loudness`
  of the song and of the game clips (a mastered song is often -4 to -8 LUFS, game sound -15 to -20). Then lower the
  music until it sits a few LU under the game sound at the moments, or let the sound lower it (`duck` with the
  music as `track`/`clips` and the game track as `under`). Never only turn the game sound down.
- A title on the moment the song sings or shouts it: `transcribe` the song with `music` true and `words` true (only
  the part you use, with `from`/`to` on a project, it is slow); whisper may hear it a bit wrong.
- See [audio.md](audio.md) for loudness: master limiter on, about -14 LUFS.

## Check
`frames` sheet of the timeline (one frame per moment) - every moment shows action, no menus or dead time. Then
render and follow [check.md](check.md).
