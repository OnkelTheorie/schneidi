# Color: correct, match, looks

## Look with numbers, not only with eyes
`scopes` on a frame (`at`) returns an image plus numbers: luma spread, clipped black/white in %, RGB means, color
cast in % (+ = too much of that channel), saturation. Measure a typical frame of every shot you will touch.

## Correct (first)
The `color` op (Color page grade) per clip, small steps:
- too dark/bright → `exposure` (stops) or `gain`; crushed/flat → `contrast` (1 = neutral) around `pivot`;
- color cast → `temperature` (+ warmer) / `tint` (+ magenta) until the cast in `scopes` is near 0;
- dull → `saturation` (50 = neutral), rarely above 60-65 for natural footage.
Neutral values: lift 0, gamma 0, gain 1, offset 25. `reset` true removes the grade.

## Match shots
Grade one shot, then `copy_attributes` from it to the others (or the same `color` values on all clips), then
check each with `scopes` and adjust the outliers - same scene, same look.

## Looks (second)
`effects` lists the built-in LUTs (`lut` in the `color` op; also `.cube`/`.3dl` files). Log footage needs its
conversion LUT first. A look on top of a bad correction stays bad.
Effects (`effect` op: blur, glow, halftone, noise, scan lines, gradient …; ids in `effects`) render after the grade,
in the clip's order as `info` lists them; the order changes the picture (blur then pixelate ≠ pixelate then blur).
`effect` with `move` ('up', 'down', 'first', 'last' or a place) reorders them.

## Green screen and hiding things
`chromakey` (Green Screen) keys the graded picture: `color` = the screen's color (pick it from a `frames` image of
the background, not pure green), `distance` = tolerance (raise it until shaded parts of the screen go, ~0.3–0.45),
`softness` = soft edge, `spill` = takes the green fringe off the subject (default full). Check with `frames`
over a contrasting clip below. Blurring a face or a plate: a `window` with fill 'blur' (or 'pixelate') on the
clip, then `track_window` so it follows the face – there is no automatic face detection. Pixelate `amount` is the
block size: 100 = 1/50 of the picture height (22 px in 1080p, enough to hide a player name), 50 = half that.
Player names in game clips (kill feed, chat, name tags above heads): `{"op":"blur_names","template":"cs2"}` (or
`valorant`, or `all` = any text, also ammo and timers) finds text-like regions over time (no OCR) and adds rectangle
windows (blur, or `style:'pixelate'`) that grow, shrink and follow moving tags; up to 8 windows per clip. 4:3 game
footage works too (black bars or a clip set to `picture_aspect` 4:3: the HUD zones move with the game's screen). It cannot
read the names, so check with `frames` (a sheet over the clip) and remove false windows (a sign on a wall, a HUD
number) with `{"op":"window","window":n,"remove":true}`; small or low-contrast tags can be missed.

## Limits
Phone footage in HDR (iPhone HLG/Dolby Vision, many Android phones) can look pale in schneidi. Raise contrast and
saturation by hand and tell the user why it looked flat.

## Check
`frames` sheet before/after (render a few frames or compare the source with the timeline) and `scopes` once more:
no clipped whites/blacks you did not intend, no color cast left.
