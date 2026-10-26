# Subtitles and captions

## Speech to text
- `transcribe` runs whisper locally. It needs the whisper extension: `extensions` without arguments shows what is
  installed. Missing → ask the user before installing (`extensions` `install` `"whisper,small"`, ~200 MB; `turbo`
  is better and bigger). Don't pass `model` unless you need a specific one - the default is the best installed.
- **Set `language`** (`de`, `en`, …): auto-detection is about twice as slow and can pick the wrong language.
- Transcribe **after** cutting, on the project: the times are then timeline times and fit the edit.
  `apply` true puts the segments onto the timeline as a new subtitle track (one undo step live in the app).
- Only the voice: `tracks` picks the voice track (e.g. `"A1"` or the stream name `"Mikrofon"`), so music and
  game sound don't confuse whisper.
- `max_chars`: 42 (one line) for landscape; ~20-28 for vertical video.

## Clean up
Read the segments. Fix names, brands and obvious mishearings with the `subtitle` op (`id`, `text`). Don't rewrite
what people said.

## Style
`track` op on the subtitle track (`ST1` …): `size`, `bold`, `outline`/`outline_width`, `box`, `y` (distance from
the bottom edge). Vertical video: bigger and higher (out of the platform buttons, see [social.md](social.md)).

## Deliver
- `render` `subtitles` `"burn"`: in the picture (social media, muted viewing).
- `"srt"`: a file next to the video (YouTube, players; viewers can switch it off).
- `format` `"srt"` writes only the subtitle file.
- From an existing SRT: the `subtitles` op (`srt`) makes a subtitle track.

## Check
`frames` at a few cue times: readable, inside the frame, not covering faces or important HUD. No cue should run
past the end of the video.
