# Automation: watch folders → highlights → render

Goal: material arrives on its own (a screen recorder, a game clip tool, OBS, a ComfyUI output folder, a camera upload)
and every new recording becomes a cut without anyone clicking - e.g. "new clip → highlight short → render".

## Set up (once)
- `watch` with `add` = the folder: its new media files come into the project's media pool on their own. `bin` puts
  them into a bin (`"Recordings"`, created if missing), `types` `video` skips sound files and images, `subfolders`
  takes those too. Only files that arrive from then on; `existing` true also takes the ones already there.
- Nothing is watched unless a folder is added (`list` shows them, `remove` stops one; imported files stay).

## Get the new files
- **One pass** (MCP tool or `schneidi-cli watch project.schneidi`): imports what is new and complete and lists it in
  `new` (file, `seconds`, `frames`, `bin`). Files still being written are `pending` - call again in a few seconds;
  a recorder writes for minutes, a file counts as complete once it stays unchanged for `settle_s` (default 3).
- **Polling**: every answer has `revision` (counts every file a watch folder brought in). Pass it back as `since`
  next time: `new` then lists everything that arrived after it, also what the app imported on its own.
- **Scripts**: `schneidi-cli watch project.schneidi --follow` keeps running and prints one JSON line per new
  complete file (stop it with Ctrl+C). Example:
  `schneidi-cli watch reels.schneidi --follow | while read -r line; do file=$(echo "$line" | jq -r .file); …; done`
- **App open**: it watches the folders itself while the project is open (one undo step per batch, the new clips
  light up in the media pool) and catches up on what came while it was closed. `@app` and the project's file both
  reach it; `--follow` does not keep the app busy between passes.

## Per new file (e.g. a highlight short)
1. `highlights` on the file (with `region` + `color` for a game's kill feed, else loud moments), `project` passed
   ([highlights.md](highlights.md)). Nothing good in it: skip the file, don't force a video out of it.
2. A new timeline per clip (`edit` op `new_timeline`) or a new project per clip (`new`, 9:16 for a short:
   [social.md](social.md)); `keep` with the best candidates' ranges.
3. `check` the project, `render` with a new name per clip (preset "Phone / Vertical 9:16" for a short).
4. Look at the result (`check` on the rendered file) before anything is uploaded; report what you made.

Keep the project small: one project for the watch folder, one timeline (or project) per output. Several files that
belong together (one match in parts) can go into one cut - wait until the last one is no longer `pending`.
