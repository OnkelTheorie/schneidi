# Export: formats, presets, exchange

## Presets (`render` `preset`)
By id or name: `youtube-1080p`, `youtube-4k`, `phone` (Phone / Vertical 9:16), `youtube-shorts` / `shorts`,
`instagram-reels` / `reels`, `tiktok` (1080x1920, up to 60 fps, AAC 48 kHz; `check` with the same `for` first),
`small`, `h265`, `av1`, `prores`, `dnxhr`, `gif`, audio only `wav`, `flac`, `aac`, `mp3`. Other parameters override
the preset. `help` `render` lists the current set with the user's own presets.

## Choosing
- Upload anywhere / play anywhere: H.264 MP4 (default), `quality` `web` (bit rate like YouTube recommends).
- Keep the recordings' quality (archive, re-upload, a master): `quality` `source` - the bit rate of the clips used,
  so the file is about as big as the source; the answer's `video_bitrate_k` says the rate aimed at.
- Smaller at the same quality: H.265 (Apple and modern players), AV1 (smallest, slower).
- `format` `s264`: schneidi's own H.264 encoder, experimental (8-bit 4:2:0 only; in two passes faster than x264
  medium on game, calm and animated footage, slower on camera footage and in one pass) - only when the user asks for
  it, never as a default.
- More editing elsewhere: ProRes or DNxHR (big files).
- `encoder` `auto` uses the graphics card when it works and falls back to software - leave it unless asked.
- `height` scales (e.g. 720), `fps` changes the frame rate, `from`/`to` renders a part, `size` forces a size
  (letterboxed - for a real vertical edit see [social.md](social.md)).
- Surround (a 5.1/7.1 project): the file gets the project's channels (`channels` `stereo` for a downmix). For TVs,
  AV receivers and Blu-ray tools `audio_codec` `ac3` (H.264/H.265/AV1 MP4, 640 kbit/s, 7.1 becomes 5.1) - only when
  the FFmpeg has the AC-3 encoder; AAC 5.1 suits upload sites.
- Subtitles: `subtitles` `burn`, `srt` or `vtt`; `embed_subtitles` true puts them into the file as switchable text
  streams ([subtitles.md](subtitles.md)).
- A version without a track (no voice-over, no music, no facecam): `mute_tracks` `["A2"]` (video tracks like `"V3"`
  are hidden) leaves it out of this render only; the project's mixer stays as it is - no mute, render, unmute.
- Machine busy (the user plays a game, records, edits): `threads` 2. A render takes all processors by default and
  holds a frame per thread in memory; a long render beside a game froze a PC.

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
