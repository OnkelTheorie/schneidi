# Templates: a finished video from a template and your clips

A **content template** is a normal schneidi project in which some clips are **slots** (placeholders): a clip slot
(video or picture), a text slot (a title's text), a music slot (the song) and a logo slot. You fill the slots with
your files and texts, schneidi fits them by the slot's rules and gives you a finished project (and video). Start
templates (`new --template`) only set format and tracks; `templates` lists both (`kind` start|content).

## Look first
- `templates` without parameters: every template with `id`, `aspect`, `length_s`, `slots`, `description`.
- `templates` `info:"<id or name>"`: each slot with its rules and where it is - `kind`, `label`, `hint` (what belongs
  there: "best kill", "product front view"), `length` fixed|flexible with `min_s`/`max_s`, `fit` (a too short clip:
  trim, speed, freeze, loop), `reframe`, `max_chars`/`shrink` (text), `fit_music`/`snap_cuts` (music) and
  `instances` (timeline, track, start_s, length_s, what it holds now). `brand_refs`: titles that take the brand
  kit's colors/fonts (see brand.md).
- Pick the clips for clip slots by their hint and length (`index`, `highlights`, `scenes`); a slot of 3 s wants a
  3 s moment: give `{file, from, to}`.

Built in: `short-hook` (9:16, hook text + 3 clips + music), `gaming-highlight` (16:9, title + 5 clips with the Edit
look + music), `product-15s` (1:1, 3 product pictures, name, claim, call to action, logo, music), `podcast-clip`
(9:16, one speaker clip, topic, sample word captions, logo - transcribe and run `captions` after filling, render with
`subtitles:"burn"`), `intro-outro` (16:9, timelines "Timeline 1" = intro 4 s and "Outro" 6 s; render each with
`timeline` for the brand kit's intro/outro).

## Fill
`templates` `fill:"<template>"`, `values:{slot: value}`, `out_project:"/abs/new.schneidi"`:
- value = `"/abs/clip.mp4"`, `{"file":"/abs/clip.mp4","from":"12.5s","to":"15.5s"}` or a text (text slots).
- The answer says per slot what was done (`adjusted`: trimmed, slowed, frozen, looped, rippled, reframed, shrunk,
  fit_music …, with `text` and `details`), `unfilled` (slots without a value keep the template's content - a
  warning; `strict:true` makes it an error), `notes` (e.g. reframed to the center because the file was not
  analysed: `reframe:"auto"` measures it, slower).
- A value that breaks a rule (missing file, no picture, text over `max_chars` without shrink) fails the call with
  its codes (FILE_MISSING, WRONG_TYPE, TOO_LONG …) and writes nothing. `dry_run:true` checks the values first and
  answers with `errors` and `unfilled` (nothing written).
- A logo slot without a value shows the brand kit's logo (the global or project kit, or `brand.logo`; not with
  `brand:"none"`).
- `brand`: the brand kit goes into the titles that reference it (default `"global"` = what applies), `"none"`
  leaves them, or kit fields like `brand set` (`{"colors":["#ffcc00"],"fonts":["Inter"]}`) = the project's own kit.
- `render:{…}` renders right away with the parameters of `render` (`out` defaults to the project name as .mp4;
  `preset` for Shorts/Reels/TikTok). Check the result with `frames` as always.
- An existing `out_project` is only replaced with `overwrite:true` (backup first). The template's own media are
  copied next to the project ("<name> media/"), `copy_media:false` keeps them in the template folder.

## In a project / in the app
- `templates` `project` + `values` fills the slots of that project in place; with `"@app"` the project open in the
  app, as one undo step.
- Edit ops: `{op:'fill', slot, value}` fills one slot; `{op:'slot', clip, id, kind?, hint?, length?, min?, max?,
  fit?, …}` marks a clip as slot (`remove:true` unmarks). `info` shows `slot:{…}` on such clips.

## Own templates and sharing
- `templates` `save:"/abs/project.schneidi"` (or `"@app"`), `name`, `description?`, `tags?`, `preview?` saves it
  as own template (used media copied into it); `rename:id` + `name`, `remove:id`.
- `export:id` + `zip:"/abs/t.zip"` writes it as ZIP; `import:"/abs/t.zip"` adds one (a name that exists gets
  "Name 2" unless `replace:true`; archives with unsafe paths or links are refused).
- Command line: `schneidi-cli template info|fill|save|import|export <value> [--param …]`.
