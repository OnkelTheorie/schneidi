---
name: schneidi
description: Edit videos with schneidi, the open-source video editor that AI can drive (MCP tools or `schneidi-cli`). Use this skill whenever someone wants to cut, trim, shorten, subtitle, color-correct, mix, reframe or export a video - a highlight reel from game or sports clips, a short/reel/TikTok in 9:16, a talking-head video or podcast without pauses, an ad or product clip, a cut in the schneidi app that is open right now - or mentions schneidi, a .schneidi project file, or the schneidi MCP tools. Also use it when video files arrive together with an editing wish, even if schneidi is not named.
---

# Editing video with schneidi

schneidi is a video editor whose every function also works without a window. You edit `.schneidi` project files
(or the project open in the app), look at the result as still images and numbers, and render the final video.

## Two ways in, same commands

- **MCP tools** (preferred when they are there): `new`, `info`, `edit`, `render`, `probe`, `frames`, … The tool names
  may carry a prefix such as `mcp__schneidi__`.
- **`schneidi-cli`** in a terminal when the tools are missing: `schneidi-cli <command> [positional] --param value`.
  Lists and objects are passed as JSON (`--ops '[{"op":"split","at":"4s"}]'`). It may print progress lines first; the
  last line is the JSON result (exit code 0 ok, 1 error, 2 usage error).

Both take the same parameters. When you are unsure how a command or edit op works, ask schneidi instead of guessing:
`help` (command `edit`, op `title`) or `schneidi-cli help edit --op title`. `effects` lists effect ids, LUTs and
transition types. These answers come from the installed version and are always current; guesses about parameter
names cost a failed call.

## How to work

1. **Look at the material first.** `probe` (length, resolution, frame rate, audio streams) and `overview` (a few
   lines per stretch of time: loudness, silence, motion, cuts, brightness), then what fits the task: `frames` (a
   contact sheet to *see* it), `transcribe` (what is said), `silence` (pauses), `scenes` (shot changes), `peaks` /
   `highlights` (loud or exciting moments), `beats` for a song (tempo, bars, phrases, breaks, drops). You cannot
   edit well what you have not looked at.
2. **Say the plan in 2-3 sentences** (length, structure, format), then go ahead. Don't wait for approval unless the
   plan rests on a guess about something important that the user can easily answer.
3. **Build it in small steps.** Create the project with `new` (match the frame rate of the main material; vertical
   = width 1080, height 1920). `new` starts from the user's default start template; name one with `template` when it
   fits the task: `podcast` (tracks Voice 1, Voice 2, Music), `vertical` (9:16), `game` (multi-stream recordings:
   the Mix stream comes in muted). `templates` lists them with the user's own. Then `edit` with a list of ops. Its answer lists the clips it added and changed
   (`changes`, with their new ids) and the timeline as a summary; take ids from there or from `info`, never guess
   them - they change when clips are split. Positions relative to a clip or to source time
   (`{"clip": 3, "source": "12.4s"}`) stay correct while earlier ops move things.
4. **Check before rendering**: `check` on the project (gaps, missing media, subtitles, sound, game sound or
   speech buried under the music, stutter in recordings; 9:16: text the apps cover, a slow start), `info` (length,
   tracks) and a `frames` sheet of the timeline.
5. **Render**, then **look at the rendered file yourself**: `check` on the file, a `frames` sheet, `probe`. Read
   [references/check.md](references/check.md) for what to look for. Fix and render again if something is off.
6. **Report briefly**: output file(s), length, what you did, measured loudness, and anything you could not do.

If an edit went wrong, `restore` puts the previous state back (every save keeps a backup; `backups` lists them).

## Live mode: the project is open in the app

When the user talks about "the open project", "in the app" or "what I'm looking at", use `@app` as the project.
Every command then works on what the app shows, unsaved changes included; `info` adds the playhead and selection,
`app` can seek, play, select, undo and save.

- One `edit` call = one undo step in the app. Put a logical change (e.g. "remove all pauses") into one call so the
  user can take it back with one Ctrl+Z.
- Don't save (`app` save) unless the user asks - they decide when their file changes.
- The app lights up the clips you changed; mention what you changed so the user knows where to look.

## Ground rules

- **Absolute paths** for every file.
- **Times**: frames (integer, in the project's frame rate), seconds like `"4.5s"`, or timecode `"00:01:02:10"`.
  Seconds are the safest. Analysis commands on a media file count frames in the file's own rate unless you pass
  `project`, so pass it when you feed the result into `edit`.
- **Never overwrite the user's material.** Renders and projects get new names (`trip-cut.mp4`, `trip-9x16.mp4`);
  put them where the user says, otherwise next to the project. Check renders that only you look at go to a
  temporary folder, never into the user's folders.
- **Decide with good defaults** instead of asking: a tight cut, -14 LUFS for the web, H.264 MP4, readable titles.
  Ask only when the answer changes the result a lot and you cannot find it in the material or the request (e.g.
  "music" requested but no music file anywhere).
- **Downloads need a yes**: speech recognition (`transcribe`) needs the whisper extension (hundreds of MB). If it
  is missing, ask before installing it with `extensions`.
- Say "done" only after you have looked at the rendered result.

## Task guides

Read the guide that fits the task (several if it combines them):

| Task | Guide |
|---|---|
| Highlight reel from game, sports or event footage | [references/highlights.md](references/highlights.md) |
| Interview, podcast, vlog, talking head: remove pauses and slips | [references/talking.md](references/talking.md) |
| Short / Reel / TikTok, 9:16, vertical | [references/social.md](references/social.md) |
| Ad, product or marketing clip, fixed length, several formats | [references/ad.md](references/ad.md) |
| Subtitles and captions | [references/subtitles.md](references/subtitles.md) |
| Color: correct, match shots, LUTs | [references/color.md](references/color.md) |
| Sound: loudness, music under speech, noise, EQ | [references/audio.md](references/audio.md) |
| Several cameras or recordings of the same event | [references/sync-multicam.md](references/sync-multicam.md) |
| Formats, presets, hardware encoding, exchange with other editors | [references/export.md](references/export.md) |
| Checking a render before handing it over | [references/check.md](references/check.md) |
