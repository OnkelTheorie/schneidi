# Check the render before handing it over

Check the rendered file, not only the project: the file is what people will see. Fix what you find, render
again, check again.

Before rendering, **`check` the project**: missing media, gaps without picture (a forgotten frame between two
clips is black in the video), subtitles too short or too fast to read or past the end, and the sound of the mix
(no sound, clipping, true peak, loudness off the target, silence at the end). Fix the errors; decide on each
warning (a fade to black or a pause can be intended). For clipping, true peak and loudness the answer has `fix`:
ready ops (master limiter, master fader) - pass them to `edit` and `check` again; a true peak above -1 dBTP is not
"ok for the codec", it clips after encoding. `target_lufs` for other targets (-23 broadcast, -16 podcast).
It also finds:
- `under_music`: a track with events (game sound, speech) more than 15 LU under the music while its events play -
  raise that track or `duck` the music under it. Name the music track "Music" if it is not a plain sound file.
- `missing_effect`: an effect this schneidi does not have (frei0r plugins of old projects) - it stays in the project
  but is skipped when rendering; remove it or use an own effect instead (`effects` lists them).
- `black_bars`: a recording has black bars (a 4:3 game in a 16:9 video) that show in a timeline of another shape
  or a zoomed clip - `{"op":"crop_bars"}` crops them; `reframe` does it by itself.
- `stutter`: a clip uses a part of a recording where the game lagged (the picture stands still) - trim the clip
  around it (`probe` `gaps` shows the jumps, `highlights` gives `clean` parts).
- 9:16 only: `safe_area` / `subtitle_safe_area` = text where TikTok, Reels and Shorts put caption and buttons
  (bottom 20 %), their button column (right) or their top bar - move it; `slow_hook` (info) = the first second
  hardly moves - start with the strongest moment.
- `pace_slow`: longer than 3 s (`max_still_s`) without a change - a cut, a zoom/position keyframe or accent, a
  text, an overlay on V2+. A warning per stretch in 9:16 videos up to 3 min (viewers swipe away), one info
  otherwise. `pace` in the answer has the numbers: `changes_per_s`, `longest_s` and where (`at_s`), a `curve` of
  changes per second, and per story section. `suggest` holds ready ops with a `why` each (a mild punch-in per
  still stretch, `text_ops` = the words spoken there as titles instead) - a suggestion, not a fix: apply it with
  `edit` only if it suits the video (a calm interview may want calm), or cut, zoom or add text yourself.
- Story: `story_hook_late` = the first strong moment (a loud hit, the first word, a text) comes after 2 s (9:16 or
  `for` a platform) or 5 s - move it to the front or tease it. With story sections on the timeline also
  `story_no_hook`, `story_open_question` (a question with no turn or resolution after it), `story_resolution_late`,
  `story_no_cta` and `story_gap` (info). `story` in the answer says what the first strong moment is. No sections yet:
  `{"op":"sections","auto":true}` suggests them from the cuts.
- After publishing: `retention` = the audience retention CSV (YouTube Studio, TikTok) → `retention_drop` (info)
  where viewers leave, with `at_s` = the change right before it: often the cut to look at.

Each finding has its time range (and `clip` when it is about one).

After rendering:

1. **`check` the file**: black stretches, frozen picture, no sound, loudness and true peak of what was really
   written. `probe` it: length as planned (ads: exact), resolution and frame rate as intended.
2. **`frames` sheet of the file** (`count` 9-16, `sheet` true), then look at it - `check` measures, only you see:
   - the subject is in frame in every shot (vertical reframes!);
   - titles, logo and subtitles are readable, spelled right, inside the frame and the safe area;
   - nothing unwanted: menus, loading screens, desktop, the end of a recording.
   For a closer look at a cut, `frames` with `at` a few frames before and after it.
   Edits to music or with kills: a review render before the real one - `render` `review:"sheet"` (out `.png`,
   seconds) or `review:"video"` (540 p, lamps light on the frame of a beat, bar one, cut, hit, kill feed line; a
   strip shows 2 s around the playhead). Read its `offsets` (`off_ms` of each hit to its nearest beat: more than
   ~40 ms is visible) and `cuts_off_beat`, fix with `align`/`trim`, review again. `review_hits` names the kill feed
   when the file name does not (`{"game":"cs2"}` or `{"region":…,"color":…}`), `beats_track` the music track.
3. **Against the request**: every requested format, length, music, logo at the end, subtitles, file names.

In the report, say what you checked and the measured numbers (length, LUFS, true peak), plus anything you could
not check (e.g. how it sounds - you only measure it).
