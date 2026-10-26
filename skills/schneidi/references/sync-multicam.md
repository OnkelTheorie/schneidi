# Several cameras or recordings of the same event

## Sync
- `sync` with the files (first = reference) returns each file's offset, found by shared sounds (claps, a round
  start, announcements). A file without a clear match is reported as not found - schneidi does not guess.
- Recordings without shared sound: `source_marker` at the same visible moment in each file, then the `multicam` op
  with `sync` `"marker"`.

## Multicam clip
- The `multicam` op creates a synced multicam clip from the files (one angle per camera/POV; it syncs by itself)
  and can place it in the timeline (`at`).
- Cut between angles: `angle` (one cut, like keys 1-9) or `angle_cuts` (many at once, e.g. switch to the POV of each
  action moment). Rename angles with `angle_name`.
- Out of sync by a few frames (drift on long recordings): `multicam_nudge` on that angle.
- `flatten` turns the multicam segments into normal clips (for effects, color per clip, export to other editors).

## Without a multicam clip
Place each recording on its own track at its `sync` offset (`place` with `at` and `track`) - good for picture-in-
picture or for a second audio recorder (camera sound replaced by the clean recorder).

## Check
`frames` with `sources` (several files on one sheet) at an event that is visible in all angles, or of the timeline
right after an angle switch: same moment in every angle?
