# Subtitles and captions

## Speech to text
- `transcribe` runs whisper locally. It needs the whisper extension: `extensions` without arguments shows what is
  installed. Missing → ask the user before installing (`extensions` `install` `"whisper,turbo"`, ~570 MB; `large`
  is the most accurate on unclear chat, ~1.1 GB, best with a graphics card; `small` ~200 MB). Don't pass `model`
  unless you need a specific one. `quality` picks a level instead: `accurate` (large-v3), `fast` (turbo) or
  `very_fast` (Parakeet, extension `parakeet`, ~670 MB: 5-8x faster for long recordings in 25 European languages,
  misses short shouts). Default: accurate with a graphics card, else fast, else the best installed.
- **Set `language`** (`de`, `en`, …): auto-detection is about twice as slow and can pick the wrong language. A
  chosen language is checked: `detected_language` and a note say when whisper clearly hears another one.
- Transcribe **after** cutting, on the project: the times are then timeline times and fit the edit.
  `apply` true puts the segments onto the timeline as a new subtitle track (one undo step live in the app).
- **Plain subtitles: add `readable` true** (with `apply`/`srt`): the words come as 1-2 line cues (≤ 42 characters a
  line, ~17 characters/s, split at sentence and phrase ends, never across a pause) that `check` accepts; without it
  whisper's one-line pieces are often flagged too fast. Don't merge cues with your own script. An existing track:
  edit op `subtitles` `{track:"ST1", readable:true}`.
- **Cue length** (`length`): `auto` (default) gives short cues on a vertical timeline (taller than wide) and normal
  ones otherwise; `short` = one line, ≤ 25 characters / 6 words, about one short sentence or less (TikTok, Shorts,
  Reels), `very_short` = 2-3 words, `normal` = as above. The answer's `length` says which was used. An existing
  track re-split: edit op `subtitles` `{track:"ST1", length:"short"}` (keeps the cues' word times; without them
  each cue's time is shared by its words' length).
- Only the voice: by default `transcribe` hears only tracks/streams named like a microphone or voice ("Mikrofon",
  "Mic/Aux", "Voice") when there is more (game, music: no song lyrics in the captions); the answer names them
  (`tracks` / `audio_stream` + a note). Choose yourself with `tracks` on a project (`"A1"`, `"Discord,Mikrofon"`,
  `"all"` = everything audible) or `audio_stream` on a file (number or name, several are mixed).
- `max_chars` (without `readable`): 42 (one line) for landscape; giving it without `length` means `normal`.

## Clean up
Read the segments. Fix names, brands and obvious mishearings with the `subtitle` op (`id`, `text`). Don't rewrite
what people said.

## Style
`track` op on the subtitle track (`ST1` …): `size`, `bold`, `outline`/`outline_width`, `box`, `y` (distance from
the bottom edge). Vertical video: bigger and higher (out of the platform buttons, see [social.md](social.md)).

## Word-by-word captions (shorts, reels, TikTok)
- One call: `transcribe` on the project with `apply` true and `captions` `"karaoke"` (the phrase stays, the spoken
  word lights up), `"word"` (one big word at a time that pops in) or `"beat"` (word by word, each word on the next
  beat: pass `beats` from `beats` with `beats` true, or `bpm` + `first_beat`). `max_words` = words per caption
  (default 4, word/beat 3).
- Already transcribed: the `captions` edit op takes `words` (the `words` list of `transcribe` with `words` true) or,
  without them, the media transcripts of the app's Transcript panel (they follow trims and speed changes).
- The new track becomes the visible one, styled big and bold with an outline, on 9:16 above the part phone apps
  cover. Change it with the `track` op: `captions` (`lines`|`karaoke`|`word`), `highlight` (colour),
  `highlight_scale`, `max_words_per_line`, `pop_ms`, `uppercase`, `shadow`, plus the text style keys.
- Fix a misheard word with the `subtitle` op (`text`): the word times stay as long as the word count stays.
- Check with `frames` at a few word times; render with `subtitles` `"burn"` (word styles exist only in the picture).

## Deliver
- `render` `subtitles` `"burn"`: in the picture (social media, muted viewing).
- `"srt"` / `"vtt"`: a file next to the video (YouTube, players; viewers can switch it off); `vtt` = WebVTT for web
  players.
- `embed_subtitles` true: every subtitle track as a text stream in the file (MP4/MOV, MKV, WebM; plain text) - also
  together with `"burn"`.
- `format` `"srt"` or `"vtt"` writes only the subtitle file.
- From an existing SRT: the `subtitles` op (`srt`) makes a subtitle track.

## Check
`frames` at a few cue times: readable, inside the frame, not covering faces or important HUD. No cue should run
past the end of the video.
