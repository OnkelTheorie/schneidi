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
Effects (`effect` op: blur, frei0r looks …) render after the grade, in the clip's order as `info` lists them; the
order changes the picture (blur then pixelate ≠ pixelate then blur). `effect` with `move` ('up', 'down', 'first',
'last' or a place) reorders them.

## Limits
Phone footage in HDR (iPhone HLG/Dolby Vision, many Android phones) can look pale in schneidi. Raise contrast and
saturation by hand and tell the user why it looked flat.

## Check
`frames` sheet before/after (render a few frames or compare the source with the timeline) and `scopes` once more:
no clipped whites/blacks you did not intend, no color cast left.
