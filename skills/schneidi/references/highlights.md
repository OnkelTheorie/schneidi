# Highlight reel (games, sports, events)

Goal: the best moments, tight, with a rising arc, often with music.

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
  - 30 s ≈ 5-8 moments. Hit the requested length within about a second.
- Short `transition`s (cross dissolve 0.2-0.3 s) or hard cuts; hard cuts on the beat feel best.
- Optional intro/outro: `title` for 1.5-2 s (name of the player/event), not longer.

## Music
- Put the music on its own audio track (`place` with `track` `"A2"` at 0, or a new track), cut it to the video
  length, `fade` out the last 1-2 s.
- Keep the game/event sound - it carries the action. Either lower the music (`volume` around -8 to -12 dB) or let
  the sound lower it (`duck` with the music as `track`/`clips` and the game track as `under`).
- See [audio.md](audio.md) for loudness: master limiter on, about -14 LUFS.

## Check
`frames` sheet of the timeline (one frame per moment) - every moment shows action, no menus or dead time. Then
render and follow [check.md](check.md).
