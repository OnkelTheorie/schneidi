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
- How a slot fits its value: a fixed slot stays as long as it is (a value left shorter there leaves a gap, and the
  next, longer value fills the whole slot again, as far as its track and its sound's track are free; a slot clip
  trimmed, cut or pasted by hand keeps the new length); a flexible one takes the value's length within `min_s`/`max_s`
  (a music slot with `fit_music`: the song's length within them) and everything after it moves - also where the
  slot is inside a compound clip (the compound clips showing it get longer/shorter with their fades; one on a
  locked track or retimed stays, with what is linked to it - a warning). A clip slot's linked sound follows its
  picture; a placeholder without sound (a picture) gets the new clip's sound on a free audio track
  (`adjusted`: sound_added). A text slot cut in two gets the text in every part.
- A value that breaks a rule (missing file, no picture, text over `max_chars` without shrink) fails the call with
  its codes (FILE_MISSING, WRONG_TYPE, TOO_LONG …) and writes nothing. `dry_run:true` checks the values first and
  answers with `errors` and `unfilled` (nothing written) - also EXISTS (the project is there, no `overwrite`) and
  IN_USE (open in the app), as the fill itself would.
- A logo slot without a value shows the brand kit's logo (the global or project kit, or `brand.logo`; not with
  `brand:"none"`).
- `brand`: the brand kit goes into the titles that reference it (default `"global"` = what applies), `"none"`
  leaves them, or kit fields like `brand set` (`{"colors":["#ffcc00"],"fonts":["Inter"]}`) = the project's own kit.
- `render:{…}` renders right away with the parameters of `render` (`out` defaults to the project name as .mp4;
  `preset` for Shorts/Reels/TikTok). Check the result with `frames` as always. If the render fails, the project is
  saved anyway: the answer has `project`, `ok:false` and `render_error {code, message}` - retry with `render` on that
  project (filling again needs `overwrite:true`).
- An existing `out_project` is only replaced with `overwrite:true` (backup first). The template's own files (media,
  LUTs, brand kit files) are copied next to the project ("<name> media/", listed in `copied_media`) once nothing
  stops the fill; a different file of that name there is never replaced (the copy gets "-2"), the same file is used
  again. `copy_media:false` keeps them in the template folder. The app does the same when such a project is saved.

## Many videos from a table
One template, a table of products/texts/clips → one project (and video) per row. `templates` `fill:"<template>"`,
`rows:"/abs/products.csv"` (or a .json file, or the rows themselves `[{slot: value, …}, …]`), `out_dir:"/abs/out"`:
```
product;hook;clip;clip.from
Mug blue;"Hot; and fast";shots/mug.mp4;2s
Pen;Writes forever;shots/pen.mp4;
```
- Header = slot ids (`templates info` lists them). Separator `,` or `;` (German Excel), UTF-8 with or without BOM,
  UTF-16 or Windows-1252 (Excel's "CSV" / "Unicode text"), quotes as in Excel. Numbers of JSON rows go into text slots
  as written (`1234567`, `2.5`). With `--map` (server) a client's path in a cell is mapped first. Text slots take the cell as text, clip/music/logo slots a file - relative paths count from the
  table's folder. `<slot>.from`/`<slot>.to` set the source range. An empty cell leaves that slot unfilled.
- Other columns are no slots (one note), but usable in the file name: `name:"{n:03}-{product}"` (`{n}` = row
  number, `{n:W}` zero padded to W = 0..9 digits). Default: an `out` or `name` column, else `<template>-001`. Names are made safe for every OS; a repeated
  name gets "-2". `values` = the same for every row (a cell wins).
- `render:{preset?, format?, …}` renders each row to `out_dir/<name>.<extension of the format>` (no `render.out`).
- The answer: per row `ok`, `project`, `video`, `adjusted` (per slot), `errors` (codes as for one fill), `totals`
  (`ok`, `failed`, `skipped`), `time_s`. A bad row (missing file …) is reported and the others are done;
  `continue_on_error:false` stops at the first. Run `dry_run:true` first: checks every row, writes nothing.
- The template's own files are copied once for the whole batch into `out_dir/<template> media/`.
- Existing projects/videos are only replaced with `overwrite:true`. Rows run one after another (each render uses all
  cores); the command line shows `{"progress":…,"stage":"row 3/20"}` on stderr.

## In a project / in the app
- `templates` `project` + `values` fills the slots of that project in place; with `"@app"` the project open in the
  app, as one undo step.
- Edit ops: `{op:'fill', slot, value, reframe?}` fills one slot; `{op:'slot', clip, id, kind?, label?, hint?, length?, min?, max?,
  fit?, …}` marks a clip as slot (`remove:true` unmarks). `info` shows `slot:{…}` on such clips. A slot made of
  several clips (frozen tail, loop repeats, a cut) changes as a whole.
- In the app a Media Pool file dropped onto a slot that can take it fills it; onto one that cannot (a text slot, a
  song onto a clip slot) it drops as always.
- Where the user finds it in the app: File › New from Template… (tiles with preview, also Import/Export of ZIPs),
  File › Save as Template…, Clip › Mark as Placeholder… (rules in the Inspector), Clip › Check Fields….

## Own templates and sharing
- `templates` `save:"/abs/project.schneidi"` (or `"@app"`), `name`, `description?`, `tags?`, `preview?` saves it
  as own template (used media, LUTs of effects and the project brand kit's logo/intro/outro copied into it); `rename:id` + `name`, `remove:id`.
  Ids match in any case; `BUSY` = another schneidi is changing the own templates right now - try again.
- `export:id` + `zip:"/abs/t.zip"` writes it as ZIP; `import:"/abs/t.zip"` adds one (a name that exists gets
  "Name 2" unless `replace:true`; archives with unsafe paths or links are refused).
- Command line: `schneidi-cli template info|fill|save|import|export <value> [--param …]`.
