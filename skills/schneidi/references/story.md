# Story: hook, sections, notes, plan and review

Goal: a video with a shape (a hook that holds, a build, a resolution, a call to action) that you checked yourself
before anyone else sees it. Everything here is an `edit` op or a `render`/`check` option; nothing runs by itself.

## Sections: the shape of the timeline
- `{"op":"sections","auto":true}` marks a first shape from the cuts: hook (the opening up to the first cut after
  1 s; at most 3 s vertical, 5 s otherwise), resolution (the last shot), build (the rest). Refine with
  `{"op":"section","kind":"cta","from":"28s","to":"30s"}` (kinds: hook, build, question, turn, resolution, cta,
  free; no overlaps: `{"op":"section","remove":true,"at":"29s"}` first). `info` lists them.
- `check` reads them: `story_no_hook`, `story_open_question` (a question section the video never answers),
  `story_resolution_late`, `story_no_cta`, `story_gap`. Fix what it says, or say why it is fine.

## Hook variants: let the user pick the opening
`{"op":"hook_variants"}` makes new timelines "Hook A/B/C" next to the edit (the original stays): A = the strongest
moment as a 2 s teaser in front (`at` = the hit_s of `highlights`, or the first marker), B = the question as a title
over the opening (`question`, else a question section or the first spoken question), C = straight into the action.
Render each short (`render` with `from`/`to`) or show `frames` sheets, then let the user choose.

## Notes: comments at a time
`{"op":"note","at":"12s","text":"zu lang?"}` leaves a comment; the answer names its id and the clip under it.
`info` lists the notes. Work through the open ones and tick them off with `{"op":"note","done":3}` - the user sees
which are done in the app.

## Plan first, then edit
When the user wants to decide before anything changes, send the ops with `plan:true` (each op with a short `why`):
nothing changes, the answer lists the steps. Show them; then `edit` with `plan_id` and `run` (`[1,3]` or `"all"`,
one undo step) or `discard:true`. A plan is void once the project changes (`PLAN_STALE`: plan again). `broll` and
`rough_cut` are best as a plan.

## Look at your own edit: the review render
Before the real render, `render` with `review` `"sheet"` (one PNG: a tile per shot, beat grid, cuts, hits, sections,
waveforms) or `"video"` (small and fast, the marks burned in). Its answer has `offsets` (each hit to its nearest beat
in ms) and `cuts_off_beat`. It changes nothing; check, fix, then render for real.

## Building blocks for the shape
- Talking: `rough_cut` (script → timeline from the best takes), `best_takes`, `hide_jumps`, `broll`, `jl_cut`,
  `room_tone` - see [talking.md](talking.md).
- Music: `fit_music` (song to the length, whole phrases out, the end kept), then `snap_to_beat` (cuts or hits on
  the beat) - see [highlights.md](highlights.md).
- Sounds: `sfx` (whoosh on a cut, riser into a drop, hit on a kill) only when the user asks for sounds, mild and
  sparse: never on every cut. `effects` with kind `sfx` lists them.
