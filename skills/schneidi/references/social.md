# Short, Reel, TikTok (vertical 9:16)

Goal: a vertical video that works on a phone, muted, in the first two seconds.

## One call: `short`
From recordings to a finished short in one step, e.g. a game clip:
`short` with `files` `["/clips/cs2/cs2_clip_….mkv"]` (CLI: `schneidi-cli short /clips/cs2/cs2_clip_….mkv`).
It runs the steps below with the usual commands and answers with `out`, `length_s`, the `moments` it used, `done`
(every step), `check` (what it fixed, what is left) and `notes`:
1. `highlights` per file - with the kill feed of `game` (`cs2`, `valorant`; by default from the folder or file name,
   `none` = loud moments only), every candidate padded to at least 6 s, the best ones within `min_s`..`max_s`
   (default 15-45 s); nothing found = the end of the recording (where a clip tool puts the moment).
2. A timeline "Short-Schnitt" with them (`keep`) in the recordings' shape, then the `aspect` copy "Short"
   (`9:16` default, `1:1`, `4:5`) with `reframe` (`auto` default, `center`, `fit`); game footage keeps its kill
   feed readable at the top (`keep`, below).
3. Word captions (`captions` `karaoke` default, `word`, `off`; `language`) - only when whisper is installed. Otherwise
   a note with the install command; it never installs: ask the user first.
4. `check` for the platform `for` (`shorts` default, `reels`, `tiktok`: length limit, -14 LUFS, true peak) and its sound
   fix (limiter, fader) applied.
5. `render` with the platform's preset into `out` (default `<first file>-short.mp4` beside it, numbered; an existing
   `out` needs `overwrite`). The project (`project`, default beside `out`) keeps both timelines for reworking.
Without `files`: `project` (also `"@app"`) and `timeline` = an existing cut to turn into the short. Look at the
result yourself afterwards (`frames` sheet of `out`) - one call does not replace that.

## Format
- New project `width` 1080, `height` 1920 (or `template` "vertical"), frame rate of the material. (`render` `size`
  only letterboxes a landscape timeline into a portrait file - that is not a vertical edit.) In an existing project
  (the one open in the app, `@app`, a 16:9 edit): a timeline of its own resolution next to it,
  `{"op":"new_timeline","name":"Short","aspect":"9:16"}` (or `{"op":"format","timeline":"Short","aspect":"9:16"}`
  for one that exists) - never `format` without `timeline` there, that turns every timeline portrait. A 16:9
  timeline placed into the short (`place_timeline`) is fitted like a 16:9 clip. Game recordings with a
  Mix stream next to the single streams: `template` "game" with `width` 1080, `height` 1920 mutes the Mix (else
  everything plays twice).
- **The short of an existing 16:9 edit in one call**: `{"op":"new_timeline","from":"<edit>","aspect":"9:16",
  "reframe":"auto"}` copies the edit (clips, titles, sound) into a new 9:16 timeline and reframes every video clip.
  Clips already in a 9:16 timeline: `{"op":"reframe"}` (all) or with `clips`/`track`.
- A 16:9 clip in a 9:16 project is fitted: full width, black above and below. Choose per clip:
  - **Auto-Reframe** (people, one subject, most footage): `{"op":"reframe","clips":[…]}` zooms to fill and writes
    x/y keyframes that keep the subject in view - it measures motion, detail and faces, holds still while the
    subject stays inside a dead zone, moves eased (no overshoot) and jumps only at scene cuts; a game's busy picture
    stays near the middle (crosshair). `smoothness` 0..1 (default 0.5; lower = follows closer, higher = calmer).
    `mode` `"center"` = fill without keyframes, `"fit"` = the blurred-background build below (also for 9:16 footage
    in a 16:9 timeline). Check the result with `frames` (a few times per clip) and fine-tune single moments with
    `keyframe` param `x`.
  - **The kill feed of game footage** (top right, cut away by a 9:16 crop): `reframe` keeps it by itself (`keep`
    `"auto"`, default) for a known game (`game` `cs2`/`valorant`, by default from the folder or file name) - a copy
    of the clip cropped to the kill feed on a free track above, centred near the top under the platforms' buttons
    (`info`: `keep_region`). `keep` `"killfeed"` = always, `"none"` = no copy (removes earlier ones), a region
    `"x,y,w,h"` (fractions of the whole picture) = any other part (a scoreboard, the minimap). Reframing a clip
    again replaces its copies.
  - **Fill and reframe by hand**: `transform` `zoom` ≈ 3.16 fills the height; `x` moves the visible window
    left/right (pixels from the center, about ±1160 at that zoom). Look with `frames` where the subject is.
  - **Fit with blurred background** (gameplay, wide shots, HUD that must stay visible): one track, one clip per
    scene, and `transform` with `fit` `"blur"` on all of them in one op
    (`{"op":"transform","clips":[…],"fit":"blur","zoom":1.3}`): the bars above and below are filled with a blurred,
    enlarged copy of the same picture. `zoom` 1.2-1.5 crops the edges of the sharp picture (more cuts off the HUD,
    e.g. the kill feed that proves a kill); `fit_blur` (0-100, default 70) and `fit_dim` (percent darker, default
    20) tune the background. Don't build it from a blurred second copy of every clip on the track below: twice the
    ops, every cut kept twice, and the preview stutters.
  - **One part of the picture, centered** (a facecam, the minimap, a kill feed): `transform` with `crop_*` keeps
    the part where it was in the picture - x/y/zoom move the whole picture around its own center. Center the part
    with `x` = -zoom * (crop_left - crop_right) / 2 and `y` = zoom * (crop_top - crop_bottom) / 2. Example, a
    1920x1080 clip in 1080x1920 (crop px = source px * 0.5625), facecam at source x 1440-1920, y 0-360, full width:
    `{"op":"transform","clips":[…],"crop_left":810,"crop_bottom":405,"zoom":4,"x":-1620,"y":-810}` (270 px left
    * zoom 4 = 1080). On a track above the gameplay this is the usual facecam-on-top layout.
  - Material that is already vertical needs nothing.
- **4:3 game footage** (many CS2/Valorant players play in 4:3): two kinds in a 16:9 recording.
  - **Black bars left and right** (pillarbox): `reframe` finds and crops them by itself (`bars` default true) and
    only the game picture fills the short; `probe` with `bars:true` shows `content` (the picture between the bars,
    in %). Outside a reframe: `{"op":"crop_bars"}` crops them on every clip (one step; then `transform` zoom).
  - **Stretched to 16:9** (wide heads, oval circles - looks like a normal 16:9 picture without bars): there is no
    reliable automatic detection; when the footage looks stretched (`frames`), `{"op":"picture_aspect",
    "aspect":"4:3"}` shows it at its true shape in preview, export and reframe (`"file"` undoes it). In a 16:9
    edit it then has room left and right (`fit:"blur"` fills it), a 4:3 timeline (`aspect:"4:3"`, 1440x1080)
    fits it exactly. Do it before `reframe` (the zoom depends on the shape). For a file whose every clip is
    stretched: `{"op":"media","media":"<file>","picture_aspect":"4:3"}` makes clips placed from it from then on
    come in at 4:3; a recorder that always stretches: `watch` `add` with `picture_aspect` `"4:3"` does that for
    each new recording of the folder (default: the file's shape).
- Keep text and faces inside the safe area: platforms cover roughly the bottom 20 % (caption, buttons), a strip
  on the right and a bar at the top. Titles and subtitles about 25-35 % up from the bottom, or in the upper third.
  `check` on the project lists text in those zones (`safe_area`, `subtitle_safe_area`).

## Content
- **Hook in the first 1-2 s**: start with the strongest moment or a bold question/claim as a `title`; the first
  frame already moves and shows the payoff, not the build-up; no slow intros, no logo first. `check` gives a
  `slow_hook` hint when the first second hardly moves, `story_hook_late` when the first hit, word or text comes
  after 2 s, and `pace_slow` for every stretch over 3 s without a cut, zoom or text.
- **Story sections** make the structure checkable: `{"op":"sections","auto":true}` suggests hook / build /
  resolution from the cuts, `{"op":"section","kind":"question","from":"4s","to":"7s"}` adds one (kinds hook, build,
  question, turn, resolution, cta, free). Not sure which opening works: `{"op":"hook_variants","at":["12.4s"],
  "question":"Wer gewinnt?"}` makes timelines "Hook A/B/C" (teaser of the moment in front / the question as a title /
  straight into the action) - compare them with `render` `review`, keep one. `short` opens with the best moment as
  its hook section by default (`hook` `"chronological"` keeps the order).
- **Notes**: "faster at 0:07" from the user becomes `{"op":"note","at":"0:07","text":"faster"}`; `info` lists open
  notes, tick each off when done (`{"op":"note","done":<id>}`).
- Length: what the user asks; otherwise 15-45 s. Cut tight, every second must earn its place.
- **Subtitles burned in** - most people watch muted ([subtitles.md](subtitles.md)): short lines (`max_chars` ~20-28),
  big font, outline or box. Word-by-word captions (karaoke or one word at a time) hold attention best: `transcribe`
  with `apply` true and `captions` `"karaoke"`.
- **Narration without a recording** (no voice in the material, a story over gameplay, a product short): `edit` op
  `speak` with the text turns it into a spoken voice clip (offline, Piper), e.g.
  `{"op":"speak","text":"Drei Kills in fünf Sekunden.","at":"0.5s","captions":"karaoke"}` - `captions` makes the word
  captions of it in the same step. Needs the extension once: `extensions` `install` `"piper,de_DE-ramona"`
  (English: `en_US-ljspeech`; ask the user first, about 90 MB). Keep lines short, one thought per `speak`; duck the
  game sound under it ([audio.md](audio.md)).
- Loud enough: about -14 LUFS ([audio.md](audio.md)).
- **Motion that keeps people watching** (use a few, not all):
  - A **progress bar** shows how much is left: `{"op":"title","kind":"progress"}` spans the whole timeline at the
    top edge (yellow bar on a dark track; `color`, `box_color`, `thickness`, `margin`, `rounded`). Keep it at the
    top: at the bottom the apps draw their own bar, caption and buttons (`check` warns). `span` `"clip"` fills over
    the title's own length instead (a countdown for one part).
  - An **animated counter** for numbers worth showing: `{"op":"title","kind":"counter","from":0,"to":1000,
    "suffix":" kills","at":"2s","duration":"1.5s","size":160}` counts up over its length (`ease` out by default,
    `decimals`, `prefix` like "$", `separators` `"comma_point"` 1,000 / `"point_comma"` 1.000 / `"space_comma"`;
    a new counter takes the UI language's and keeps them). Same style fields as a text title.
  - **Pushes and whip pans** between shots instead of dissolves: `transition` with `type` `push_left`/`push_up` …
    (the new shot pushes the old one out) or `whip_left` … (a fast push with motion blur; `blur` 0-100), 0.2-0.4 s
    on the beat. `ease` (`in_out` is the default for whips) shapes the speed; wipes (`wipe_*`, `softness`) work too.

## Render
`render` with `preset` `"shorts"`, `"reels"` or `"tiktok"` (YouTube Shorts, Instagram Reels, TikTok: 1080x1920
H.264 in web quality, the timeline's frame rate up to 60 fps, AAC 48 kHz) - `"phone"` is the same without the
60 fps limit. `subtitles` `"burn"` if the subtitles are on a subtitle track. Before it, `check` with the same
`for` (the platform): `too_long` = longer than the platform takes as a short (Shorts and Reels 3 min, TikTok 10 min), and the
sound fix for -14 LUFS / -1 dBTP. Then [check.md](check.md): is the subject in frame in every shot, is all text inside
the safe area?
