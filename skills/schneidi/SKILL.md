---
name: schneidi
description: Edit videos with schneidi, the open-source video editor that AI can drive (MCP tools or `schneidi-cli`). Use this skill whenever someone wants to cut, trim, shorten, subtitle, color-correct, mix, reframe or export a video - a highlight reel from game or sports clips, a short/reel/TikTok in 9:16, a talking-head video or podcast without pauses, an ad or product clip, a cut in the schneidi app that is open right now, a video from a template or many videos from one template and a table (CSV), the same logo/colors/intro/outro on every video - or mentions schneidi, a .schneidi project file, or the schneidi MCP tools. Also use it when video files arrive together with an editing wish, even if schneidi is not named.
---

# Editing video with schneidi

schneidi is a video editor whose every function also works without a window. You edit `.schneidi` project files
(or the project open in the app), look at the result as still images and numbers, and render the final video.

## Two ways in, same commands

- **MCP tools** (preferred when they are there): `new`, `info`, `edit`, `render`, `probe`, `frames`, … The tool names
  may carry a prefix such as `mcp__schneidi__`.
- **`schneidi-cli`** in a terminal when the tools are missing: `schneidi-cli <command> [positional] --param value`.
  Lists and objects are passed as JSON (`--ops '[{"op":"split","at":"4s"}]'`). Progress of long commands goes to
  **stderr** as lines like `{"progress":42,"stage":"transcribe","eta_s":30}` (step running now, rough seconds left);
  **stdout** carries only the JSON answer, and its **last line** is the
  result (exit code 0 ok, 1 error with `{"error":…}`, 2 usage error). Read the answer from stdout's last line.

Both take the same parameters. When you are unsure how a command or edit op works, ask schneidi instead of guessing:
`help` (command `edit`, op `title`) or `schneidi-cli help edit --op title`. `effects` lists effect ids, LUTs and
transition types. These answers come from the installed version and are always current; guesses about parameter
names cost a failed call.

## How to work

1. **Look at the material first.** `probe` (length, resolution, frame rate, audio streams) and `overview` (a few
   lines per stretch of time: loudness, silence, motion, cuts, brightness), then what fits the task: `frames` (a
   contact sheet to *see* it), `transcribe` (what is said), `silence` (pauses), `scenes` (shot changes), `peaks` /
   `highlights` (loud or exciting moments), `beats` for a song (tempo, bars, phrases, breaks, drops). You cannot
   edit well what you have not looked at. With a lot of material (vlog, interview, B-roll) build the material
   index once: `index` with the files or the project lists every shot with its quality (sharp/blurry,
   dark/overexposed, motion, pan, face likely; counted faces with the `faces` extension), speech and sounds, and shows the undescribed shots as a sheet -
   describe them with `annotate` ([{file, shot, desc, tags}], kept for good), then `index` `query` ("ruhige
   Totale vom Laden", "wo lacht er") finds them, with a project also in timeline time. `peaks {events:true}` also
   says what is heard: speech, music, laughter, applause, cheering, shots, screams, silence.
2. **Say the plan in 2-3 sentences** (length, structure, format), then go ahead. Don't wait for approval unless the
   plan rests on a guess about something important that the user can easily answer.
3. **Build it in small steps.** Create the project with `new` (match the frame rate of the main material; vertical
   = width 1080, height 1920). `new` starts from the user's default start template; name one with `template` when it
   fits the task: `podcast` (tracks Voice 1, Voice 2, Music), `vertical` (9:16), `game` (multi-stream recordings:
   the Mix stream comes in muted). `templates` lists them with the user's own and the **content templates**:
   finished projects with slots for clips, texts, music and logo (`short-hook`, `gaming-highlight`, `product-15s`,
   `podcast-clip`, `intro-outro`, the user's own). When one fits the request (or the user names one), fill it instead
   of building from scratch: `templates` `fill` gives a finished project in one call, with `rows` one per line of a
   table ([templates.md](references/templates.md)). Then `edit` with a list of ops. Its answer lists the clips it added and changed
   (`changes`, with their new ids) and the timeline as a summary; take ids from there or from `info`, never guess
   them - they change when clips are split. Positions relative to a clip or to source time
   (`{"clip": 3, "source": "12.4s"}`) stay correct while earlier ops move things. Give an op `"why"` (a short
   reason, e.g. `"hook: loudest kill first"`): the answer repeats it under `why`, so the user can follow the cut.
   **Plan first** when the user wants to decide before anything changes ("show me what you would cut"): `edit` with
   `plan:true` checks every op like `dry_run`, changes nothing and answers `plan {id, steps:[{n, op, why, summary,
   changes}]}`; show the steps, then `edit` with `plan_id` and `run` (`[1,2,4]` or `"all"`: one edit, one undo step)
   or `discard:true`. A plan is void once the project changes (`PLAN_STALE`: plan again).
4. **Check before rendering**: `check` on the project (gaps, missing media, subtitles, sound, game sound or
   speech buried under the music, stutter in recordings, too long without a cut, zoom or text (`pace`), a late
   hook and story sections; 9:16: text the apps cover, a slow start), `info` (length,
   tracks) and a `frames` sheet of the timeline. With music or kills, a **review render** shows what you would
   otherwise guess: `render` with `review` `"sheet"` (one PNG: a tile per shot, beat grid, cuts, hits, sections,
   waveforms) or `"video"` (small, fast, the marks burned in), and its answer has `offsets` (each hit next to its
   nearest beat in ms) and `cuts_off_beat`. It changes nothing in the project.
5. **Render**, then **look at the rendered file yourself**: `check` on the file, a `frames` sheet, `probe`. Read
   [references/check.md](references/check.md) for what to look for. Fix and render again if something is off.
6. **Report briefly**: output file(s), length, what you did, measured loudness, and anything you could not do.

If an edit went wrong, `restore` puts the previous state back (every save keeps a backup; `backups` lists them).

## Live mode: the project is open in the app

When the user talks about "the open project", "in the app" or "what I'm looking at", use `@app` as the project.
Every command then works on what the app shows, unsaved changes included; `info` adds the playhead and selection,
`app` can seek, play, select, undo and save, and `open` shows files there (a recording goes into the app's media
pool and is selected; a .schneidi project opens, never over unsaved changes).

- **The result belongs in the open project**, not in a project file beside it that the user never sees: a new
  version (another format, a short from a long edit) goes into a new timeline with its own resolution
  (`new_timeline` with `aspect` `"9:16"`, or `format` with `timeline`): the 16:9 edit next to it stays as it is.
  A 9:16 copy of an edit with every clip following its subject: `new_timeline` with `from`, `aspect` `"9:16"` and
  `reframe` `"auto"` ([social.md](references/social.md); 4:3 game footage with black bars or stretched to 16:9:
  `crop_bars`, `picture_aspect` there).
  `format` without `timeline` changes the project's resolution and with it every timeline that has none of its own:
  before that while other timelines hold clips, ask in one sentence. In the app `format` refuses such a change until
  you send `confirm:true` - only after the user said yes.
- One `edit` call = one undo step in the app. Put a logical change (e.g. "remove all pauses") into one call so the
  user can take it back with one Ctrl+Z.
- Don't save (`app` save) unless the user asks - they decide when their file changes.
- The app lights up the clips you changed; mention what you changed so the user knows where to look.
- **The user edits too.** Between your answers they trim, move, mute or delete clips by hand (in the app chat a
  "[Note from the schneidi app: the user changed …]" lists it). Build on their changes: when you rework something
  they adjusted, change the existing clips (`trim`, `move`, volume) instead of deleting and placing them again - a
  new `place` loses their trims, fades and levels. Ids you remember may be gone; take them from `info`.

## Ground rules

- **Absolute paths** for every file.
- **Times**: frames (integer, in the project's frame rate), seconds like `"4.5s"`, timecode `"00:01:02:10"`
  (hours:minutes:seconds:frames), or clock time `"1:30"` (minutes:seconds), `"00:01:02.5"` (decimal seconds).
  Seconds are the safest. Analysis commands on a media file count frames in the file's own rate unless you pass
  `project`, so pass it when you feed the result into `edit`.
- **The global brand kit and the user's own templates are theirs**: change, replace or remove them only on request
  (a project's own kit or a new project from a template is fine).
- **Never overwrite the user's material.** Renders and projects get new names (`trip-cut.mp4`, `trip-9x16.mp4`);
  put them where the user says, otherwise next to the project. Check renders that only you look at go to a
  temporary folder, never into the user's folders.
- **Do what was asked, the way it was asked.** "On every kill" means on every kill, "shorter" means shorter. If you
  think another way is better, do the request first and offer the other way in one sentence - not the reverse.
- **Decide with good defaults** instead of asking: a tight cut, -14 LUFS for the web, H.264 MP4, readable titles.
  Ask only when the answer changes the result a lot and you cannot find it in the material or the request (e.g.
  "music" requested but no music file anywhere).
- **Downloads need a yes**: speech recognition (`transcribe`) needs the whisper extension (hundreds of MB). If it
  is missing, ask before installing it with `extensions`. The same for the `x264` extension (72 MB, GPL): when
  `check` reports encoder_quality or `render` answers with `encoder_hint` (no graphics card encoder, so H.264/H.265
  are encoded with the free OpenH264/kvazaar at about twice the file size), offer it; with a yes run `extensions`
  install x264 and render again. Spoken text (`edit` op `speak`) needs `piper` plus a voice (about 90 MB). Counted
  faces in `index` need the `faces` extension (YuNet, under 1 MB, plus ONNX Runtime 6 MB on Linux, 66 MB on Windows).
- Say "done" only after you have looked at the rendered result.

## Task guides

Read the guide that fits the task (several if it combines them):

| Task | Guide |
|---|---|
| Highlight reel from game, sports or event footage | [references/highlights.md](references/highlights.md) |
| Interview, podcast, vlog, talking head: remove pauses and slips | [references/talking.md](references/talking.md) |
| Short / Reel / TikTok, 9:16, vertical (one call: `short`) | [references/social.md](references/social.md) |
| Ad, product or marketing clip, fixed length, several formats | [references/ad.md](references/ad.md) |
| Subtitles and captions | [references/subtitles.md](references/subtitles.md) |
| Color: correct, match shots, LUTs | [references/color.md](references/color.md) |
| Sound: loudness, music under speech, noise, EQ | [references/audio.md](references/audio.md) |
| Several cameras or recordings of the same event | [references/sync-multicam.md](references/sync-multicam.md) |
| Formats, presets, hardware encoding, exchange with other editors | [references/export.md](references/export.md) |
| Checking a render before handing it over | [references/check.md](references/check.md) |
| Automation: new recordings in a folder → cut → render, without anyone clicking | [references/automation.md](references/automation.md) |
| Story: hook, sections, notes, plan first, review render | [references/story.md](references/story.md) |
| Brand kit: the same logo, colors, fonts and intro/outro on every video | [references/brand.md](references/brand.md) |
| Templates: a finished video from a template's slots (clips, texts, music, logo), many videos from a table (CSV), save/share own templates as ZIP | [references/templates.md](references/templates.md) |
