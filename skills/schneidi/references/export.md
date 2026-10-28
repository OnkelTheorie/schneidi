# Export: formats, presets, exchange

## Presets (`render` `preset`)
`YouTube 1080p`, `YouTube 4K`, `Phone / Vertical 9:16`, `Small (for sharing)`, `H.265 (small file, high quality)`,
`AV1 (very small file)`, `High Quality / Archive (ProRes 422 HQ)`, `Editing Exchange (DNxHR HQ)`, `Animated GIF
(480p, 15 fps)`, audio only (WAV, FLAC, AAC, MP3). Other parameters override the preset. `help` `render` lists the
current set.

## Choosing
- Upload anywhere / play anywhere: H.264 MP4 (default), `quality` 3 = web (bit rate like YouTube recommends).
- Smaller at the same quality: H.265 (Apple and modern players), AV1 (smallest, slower).
- More editing elsewhere: ProRes or DNxHR (big files).
- `encoder` `auto` uses the graphics card when it works and falls back to software - leave it unless asked.
- `height` scales (e.g. 720), `fps` changes the frame rate, `from`/`to` renders a part, `size` forces a size
  (letterboxed - for a real vertical edit see [social.md](social.md)).
- Subtitles: `subtitles` `burn` or `srt` ([subtitles.md](subtitles.md)).
- A version without a track (no voice-over, no music, no facecam): `mute_tracks` `["A2"]` (video tracks like `"V3"`
  are hidden) leaves it out of this render only; the project's mixer stays as it is - no mute, render, unmute.

## Files
New, clear names; never the name of an input file. `overwrite` true only for your own earlier render of the same
job. Several timelines in one project: `timeline` picks which one.

## Exchange with other editors
`export_timeline` writes FCP7 XML (Premiere, DaVinci), OTIO (DaVinci 18.5+) or EDL; `warnings` lists what does not
carry over (titles, effects, color). `import` with a timeline file brings a cut from those editors into schneidi.

## Weak computers
Editing in the app: proxies (`app` `proxies`) make playback smooth; renders always use the original files.
`app` `clean_proxies` lists proxy files no open or recent project needs and the space they take; with `delete: true`
it deletes them (ask the user first).

## Afterwards
[check.md](check.md).
