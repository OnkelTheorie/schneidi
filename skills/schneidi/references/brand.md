# Brand kit: logo, colors, fonts, intro/outro

The user sets their brand once: a logo (with a corner and size), up to 4 colors, 2 fonts (heading, body) and an
intro/outro clip. There is one **global** kit (all projects; app: Workspace › Brand Kit) and a project may have its
**own** that replaces it. An empty kit is the default: nothing is branded unless a title references the kit.

## Look first
`brand` with the project shows `global`, `project_kit` (null = none), `effective` (what applies) and `references`
(how many titles and subtitle tracks use the kit). `missing_files` names kit files that are not there.

## Titles that follow the kit
In a `title` op (and a subtitle track's style) give `color`, `outline_color` or `box_color` as `"brand1"`..`"brand4"`
and `font` as `"brand1"`/`"brand2"`: `{"op":"title","at":0,"text":"Summer sale","color":"brand1","font":"brand1"}`.
The title takes the kit's value now and keeps it stored, so the render never depends on the kit; when the kit changes,
`brand` with `action` `"apply"` copies the new values into every referencing title (one undo step). A fixed value
(`"#ffffff"`) ends the reference. A reference to a color the kit does not have keeps the title's value. A brand text
color replaces an animated one (its `title_color` keyframes are removed); keyframing `title_color` again makes the
color fixed. `info`
shows references as `color_ref` … `"brand2"`.

## Changing the kit
- `brand` `action` `"set"` with the fields to change (`logo`, `logo_corner` top-right|top-left|bottom-right|bottom-left|
  center, `logo_size` 0.01-1 of the picture width, `colors` "#ffcc00,#202020", `fonts` "Inter,Source Serif",
  `intro`, `outro`; `""` clears one). Default: the global kit; with `global:false` the project's own (it starts as
  a copy of what applied). With a project the kit is applied right away.
- `action` `"unset"`: the project uses the global kit again.
- Change the global kit only when the user asked for it - it is theirs, for every project.

## Logo and intro/outro in the timeline
Only on request (never by themselves): `action` `"logo"` puts the logo as a clip on a new top video track over the
whole timeline at the kit's corner and size; `action` `"intro_outro"` puts the intro at the start (everything on
unlocked tracks moves right by its length) and the outro after the end, on V1/A1. Both take `timeline`. Check the
result with a `frames` sheet: a logo must not cover text or the parts a phone app covers in 9:16 (`check`).
