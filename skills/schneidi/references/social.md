# Short, Reel, TikTok (vertical 9:16)

Goal: a vertical video that works on a phone, muted, in the first two seconds.

## Format
- New project `width` 1080, `height` 1920 (or `template` "vertical"), frame rate of the material. (`render` `size`
  only letterboxes a landscape timeline into a portrait file - that is not a vertical edit.) With the project open
  in the app (`@app`): a new timeline there instead, and ask before `format` turns a landscape project with other
  timelines portrait (see the guide's live mode; `format` there needs `confirm:true` after the user's yes). Game recordings with a
  Mix stream next to the single streams: `template` "game" with `width` 1080, `height` 1920 mutes the Mix (else
  everything plays twice).
- A 16:9 clip in a 9:16 project is fitted: full width, black above and below. Choose per clip:
  - **Fill and reframe** (people, one subject): `transform` `zoom` ≈ 3.16 fills the height; `x` moves the visible
    window left/right (pixels from the center, about ±1160 at that zoom). Look with `frames` where the subject is.
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
- Keep text and faces inside the safe area: platforms cover roughly the bottom 20 % (caption, buttons), a strip
  on the right and a bar at the top. Titles and subtitles about 25-35 % up from the bottom, or in the upper third.
  `check` on the project lists text in those zones (`safe_area`, `subtitle_safe_area`).

## Content
- **Hook in the first 1-2 s**: start with the strongest moment or a bold question/claim as a `title`; the first
  frame already moves and shows the payoff, not the build-up; no slow intros, no logo first. `check` gives a
  `slow_hook` hint when the first second hardly moves.
- Length: what the user asks; otherwise 15-45 s. Cut tight, every second must earn its place.
- **Subtitles burned in** - most people watch muted ([subtitles.md](subtitles.md)): short lines (`max_chars` ~20-28),
  big font, outline or box.
- Loud enough: about -14 LUFS ([audio.md](audio.md)).

## Render
`render` with `preset` `"Phone / Vertical 9:16"` (H.264, web quality), `subtitles` `"burn"` if the subtitles are on
a subtitle track. Then [check.md](check.md): is the subject in frame in every shot, is all text inside the safe area?
