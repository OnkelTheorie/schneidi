# schneidi

**A fast, light video editor that an AI assistant can edit with you – live, in the project you have open.**

schneidi has four pages – Media, Edit, Color and Deliver – and fewer features than the big editors, but it starts in under a second, plays a demanding 1080p60 project smoothly on four CPU cores and runs even on a Raspberry Pi. It opens H.264/H.265 video with AAC sound on Linux out of the box, which the free DaVinci Resolve does not. The UI is available in English, German, Spanish, French, Polish and Russian.

[![Download](https://img.shields.io/github/v/release/OnkelTheorie/schneidi?label=Download&color=2ea44f)](https://github.com/OnkelTheorie/schneidi/releases/latest)

**[Download the latest release](https://github.com/OnkelTheorie/schneidi/releases/latest)** (Linux AppImage or Debian package). Windows: no download yet – [build it yourself](#building) (tested less thoroughly than Linux); an installer is planned for 1.0.

> Status: early development (0.x). The project file format may still change between versions.

<p align="center">
  <a href="https://github.com/ZenITDigital"><img src="https://github.com/ZenITDigital.png?size=160" width="80" height="80" alt="ZenITDigital"></a><br>
  <b>💛 A huge thank you to <a href="https://github.com/ZenITDigital">@ZenITDigital</a></b><br>
  for sponsoring the Claude Max plan that schneidi is developed with.
</p>

![An AI assistant edits live in schneidi: keeps the speech, adds b-roll with dissolves, subtitles, a title, ducked music and a grade](screenshots/demo.gif)

## Why schneidi?

- **Edit together with an AI.** schneidi comes with its own command line and [MCP server](#command-line-and-ai-editing-mcp). Claude or another assistant can find pauses, scene changes and loud moments, transcribe, cut, grade, mix and render. When the project is open in the app, each change lands there as one undo step while you watch ("AI connected") – review it, undo it or disconnect.
- **A professional workflow on any computer.** Pages for each step, ripple, roll, slip and slide trims, compound clips, multicam, color wheels and scopes – with the shortcuts you know from the big editors (presets for DaVinci-style or beginner keys), but light enough for an older laptop.
- **Everything stays on your computer.** Speech recognition (whisper.cpp), text-based editing, sync by sound and rendering work offline – no account, no cloud, open source (GPL-3.0).
- **Long recordings of one event.** Sync hours of recordings from several PCs or cameras by their sound and cut them in a multicam view that shows up to 16 angles at once.

## Screenshots

![Edit page while an AI assistant is connected: transcript, title inspector and timeline](screenshots/edit.png)

| | |
|---|---|
| [![Media page: Media Storage and Media Pool](screenshots/media.png)](screenshots/media.png)<br>**Media** – browse folders, import into the Media Pool | [![Color page: a window darkens everything outside an ellipse, color wheels and RGB parade](screenshots/color.png)](screenshots/color.png)<br>**Color** – color wheels, windows, LUTs, scopes |
| [![Deliver page: render settings and render queue](screenshots/deliver.png)](screenshots/deliver.png)<br>**Deliver** – presets and render queue | [![Multicam view: nine angles of one event side by side](screenshots/multicam.png)](screenshots/multicam.png)<br>**Multicam** – click an angle or press 1–9 to cut to it |

## Features

- Multiple video and audio tracks: cut, trim, move, snapping, undo/redo
- **Multicam:** a multicam clip from several recordings of one event, synced by sound, markers, In points or start; the viewer shows up to 16 angles at once (more on pages) – click or press 1–9 to cut, change angles later, flatten to normal clips
- Timeline zoom and scroll change **only on user input** – nothing jumps when clips get shorter
- Fully customizable keyboard shortcuts and mouse side buttons, two per function – presets (DaVinci-style or beginner) and whole sets to save and load; panel sets for the Edit page, panel sizes remembered per page
- Titles, subtitles, fades, transitions, volume, loudness normalization
- Color correction, curves, LUTs, effects with keyframes; copy and paste grades (`=`/`-`)
- **Windows** on the Color page (rectangle, ellipse, soft edge, invert): grade only part of the picture, or blur, pixelate or cover a region (a face, a name, an overlay); **tracking** makes a window follow a moving object
- Video stabilization (vid.stab) and audio noise reduction
- frei0r effects; import effect presets from Kdenlive and Shotcut
- Video scopes: waveform, RGB parade, vectorscope, histogram
- Two-up/four-up trim view while dragging an edit
- **Own audio engine (SMT):** sound is read, mixed and played sample-exact – lips in sync within about 1 ms in the viewer, no clicks when stopping, jumping or moving faders, real 24-bit export; choose the output device, survives an unplugged headset
- Loudness and true-peak meters, master limiter that holds a true-peak ceiling
- Per-track EQ (6 bands) and compressor in the mixer, ducking (music goes down while someone speaks)
- Files with several audio streams (OBS): import presets per stream title – mute the mix, leave out streams, name the tracks
- Clip speed / retime
- **Timeline exchange:** import and export FCP7 XML, OpenTimelineIO and EDL – bring a cut over from Premiere Pro or DaVinci Resolve, or hand one over for finishing
- Own video decoding, also on the graphics card (NVIDIA, AMD/Intel via VA-API); proxy media and a render cache for smooth playback – schneidi offers proxies when playback cannot keep up
- Export and render queue via FFmpeg: H.264, H.265, AV1, VP9 (WebM), ProRes and DNxHR, animated GIF, PNG/TIFF/JPEG image sequences and audio only (AAC, MP3, Opus, WAV, AIFF, FLAC, Apple Lossless); hardware encoders (NVENC, VAAPI, Quick Sync) when available; YouTube/phone presets cap the bit rate; **Smart Render** copies unchanged parts of H.264/H.265 recordings instead of encoding them again
- Subtitles from speech (whisper.cpp, offline) – pick the voice tracks, one subtitle track each; an optional download under Workspace → Extensions
- Text-based editing: the Transcript panel shows what is said – delete words to cut them, remove pauses
- **Command line and MCP server** (`schneidi-cli`): scripts or an AI assistant can edit almost everything the app can – find pauses, scene changes, loud moments and events in a part of the picture (kill feed, score), pick highlights (your own kills, loud fights), sync recordings by their sound, build multicam clips, transcribe, cut and trim, grade, animate, mix, duck music and normalize, measure loudness, look at frames (or a detail of them) and scopes, render ([details](#command-line-and-ai-editing-mcp))
- Themes, six UI languages (English, German, Spanish, French, Polish, Russian)

## How light is it?

Measured with 0.11 on a Ryzen 5 5600X restricted with Linux cgroups (cores, RAM, no swap) – a stand-in for old hardware, not a real old laptop.

**Starting:**

| | Empty project | Project with a 1080p clip |
|---|---|---|
| Least RAM it starts and keeps running with | 128 MB | 288 MB |
| Window visible, one full core | 0.6 s | 0.5 s |
| Window visible, 10 % of one core | 6 s | 6 s |
| Window visible, 5 % of one core | 11 s | 15 s |

Thumbnails for the Media Storage (your Videos folder) are made by a separate ffmpeg at the start; for a large file that takes up to ~100 MB more for a moment.

**But starting is not the same as editing smoothly.** Playback of a demanding project (12 gaming clips, 1080p60 at 140 Mbit/s, a grade on every clip), frames per second out of 60:

| CPU | From the original files | Originals, decoded on the graphics card | From proxies |
|---|---|---|---|
| 1 core | 12 – stutters | 55 | 57 |
| 2 cores | 44 | 59 – smooth | 57 |
| 4 cores | 57 – smooth | | |
| 6 cores | 58 – smooth | | |

0.10 managed 3.6 frames/s on one core and 9 on two: 0.11 decodes the video itself instead of through MLT. Decoding on the graphics card (NVIDIA, AMD/Intel via VA-API; here a GTX 1070) is on by default. On a computer too slow for the originals, schneidi notices it during playback and offers to generate proxy media (also by hand: right-click a clip in the Media Pool › Generate Proxy Media).

**And on real weak hardware: a Raspberry Pi Zero 2 W** (4 × 1 GHz Cortex-A53, 512 MB RAM, Debian 13 ARM64, no hardware decoding; built from source, there is no ARM download yet). DaVinci Resolve, Premiere Pro and Final Cut Pro are not available for this computer at all. Material: 15 s of a 1080p60 gaming clip.

| | Raspberry Pi Zero 2 W |
|---|---|
| Window visible | 6.5 s, 180 MB RAM |
| Playback 720p30 original | 29 of 30 frames/s, sound in sync |
| Playback 1080p60 original | 1 frame/s – the Pi cannot even decode it in real time |
| Playback 1080p60 from proxies | 59 of 60 frames/s, sound in sync |
| Jumping in the timeline, original → proxies | 2 s → 0.07 s |
| Export 5 s of 720p (x264) | 82 s |

## Command line and AI editing (MCP)

`schneidi-cli` edits the same project files without a window, for scripts and AI assistants. A typical rough cut:
let the AI find the pauses, keep the rest, look at the result, render, then fine-tune in schneidi.

```bash
schneidi-cli new talk.schneidi --fps 25 --media talk.mp4
schneidi-cli silence talk.mp4 --project talk.schneidi        # "sound" ranges without the pauses
schneidi-cli scenes broll.mp4 --project talk.schneidi        # shot changes
schneidi-cli scenes game.mp4 --region 0.75,0.05,0.25,0.3     # events in a part of the picture (kill feed)
schneidi-cli peaks game.mp4 --top 10                         # the loudest moments
schneidi-cli highlights game.mp4 --region 0.70,0.04,0.22,0.23 --color "#e7e371"  # own kills (Valorant), best first
schneidi-cli sync cam1.mp4 cam2.mp4 cam3.mp4                 # offsets of recordings of the same event
schneidi-cli extensions --install whisper,small              # optional speech recognition (download, ~200 MB)
schneidi-cli transcribe talk.mp4 --language de --words       # what is said, with word times
schneidi-cli edit talk.schneidi --ops '[{"op":"keep","media":"talk.mp4","ranges":[[0,250],[310,900]]}]'
schneidi-cli frames talk.schneidi --count 9 --sheet --out sheet.jpg
schneidi-cli scopes talk.schneidi --at 10s --out scopes.png      # waveform, parade, vectorscope + numbers
schneidi-cli loudness talk.schneidi                          # LUFS, true peak of the mix
schneidi-cli render talk.schneidi --out talk-cut.mp4 --preset "YouTube 1080p"
schneidi-cli export_timeline talk.schneidi --out talk.xml       # for Premiere / DaVinci (also .otio, .edl)
schneidi-cli import talk.schneidi --timeline_file premiere.xml   # a cut from Premiere / DaVinci as a new timeline
schneidi-cli restore talk.schneidi                           # undo the last change (file mode)
schneidi-cli help                                            # all commands (JSON)
schneidi-cli help edit                                       # every edit operation with details
schneidi-cli help edit --op speed_ramp                       # one edit operation
```

Edit operations cover cutting, inserting and source edits (insert, replace, place on top, fit to fill), moving,
trimming (ripple, roll, slip, slide), titles with their style, fades, transitions (dissolve, dip, wipes, luma),
speed and speed ramps, markers, subtitles (SRT or cue lists, track styles), effects and presets, the color grade
(wheels, LUTs, windows with blur/pixelate and tracking), transform and crop, keyframes for any animatable value, paste attributes, volume/pan, the mixer
(also to a loudness target, EQ and compressor per track), ducking, loudness normalization, tracks, linking, compound
clips, multicam clips (create, cut to an angle, change angles, flatten), several timelines and the project format. Positions can be given relative to a clip or to the source material (`{"clip": 12, "source":
"34.2s"}`), so they stay right while earlier operations move clips. Output is JSON on stdout (exit code 0 = ok,
1 = error, 2 = usage error); every save keeps a backup of the previous state (`backups`, `restore`). `schneidi-cli mcp` runs the same commands as an [MCP](https://modelcontextprotocol.io) server on stdio.

**Connect Claude in one step:** `schneidi-cli setup claude` (AppImage: `schneidi.AppImage cli setup claude`) adds the
MCP server to Claude Code and Claude Desktop with the path of this installation, and installs the editing guide as a
skill for Claude Code ([skills/schneidi](skills/schneidi/SKILL.md): workflow and how-tos for highlight reels, talking
heads, shorts, ads, subtitles, color, sound). `--only code` / `--only desktop` picks one, `--remove` takes it out again.
Every other AI gets the same guide through the `guide` tool. By hand: `claude mcp add --scope user schneidi --
schneidi-cli mcp`, or in Claude Desktop's `claude_desktop_config.json`:

```json
{ "mcpServers": { "schneidi": { "command": "/path/to/schneidi-cli", "args": ["mcp"] } } }
```

**Live with the app:** if the project is open in schneidi, every command works on what the app shows (unsaved
changes included). An `edit` changes the open project as one undo step and does not save; `info` adds the playhead
and the selection; `app` plays, seeks, selects, undoes, saves and generates proxies. The app shows "AI connected"
while a session is active, lights up the clips it changed and has a button to disconnect it. `@app` as the project
means the project of the running app. Without the app (or on a server) the CLI works on the file as before.

```bash
schneidi-cli app state                                       # playhead, selection, open timeline
schneidi-cli edit @app --ops '[{"op":"split","at":"12s"}]'   # cut in the open project
schneidi-cli app undo
```
The AppImage contains the CLI too: `./schneidi-<version>-x86_64.AppImage cli help`, or a link to the AppImage named `schneidi-cli`.

## Roadmap

What we are working on next. Plans can still change; the order within a version is not fixed.

### 0.11.x – bug fixes
Only fixes for problems found in 0.11.0 – new features wait for the next version.

### 0.12 – AI Update
| Feature | What it does |
|---|---|
| **AI chat in the app** | A chat panel next to the timeline that edits live with the same tools as the MCP server – Claude, OpenAI-compatible or local (Ollama); let it work directly or review its changes and accept them as one undo step |
| **Claude skill/plugin** | One command installs everything Claude needs to edit with schneidi |
| **Server version** | schneidi on a server or in Docker, for fully automatic editing through MCP, CLI or HTTP |
| **Update notice** | Once a day schneidi asks GitHub for a new release – can be switched off, no error without a network |

### Later
- **0.13 – Everyday Update:** change the order of effects, slip/slide tools, color grades shown in the filmstrip and Media Pool, Media Pool filters
- **0.14 – Shorts Update:** auto-reframe between 16:9 and 9:16, word-by-word captions, presets for Shorts/Reels/TikTok, voiceover recording, text to speech
- **0.15 – Template Update:** templates with placeholders filled from a list, brand kit
- **0.16 – Newcomer Update:** Premiere/Final Cut key presets, export for Shotcut/Kdenlive and Final Cut, apt repository

## Building

C++20, Qt 6 Widgets, MLT 7, FFmpeg libraries (own audio engine), CMake.

**Linux (Debian/Ubuntu):**

```bash
sudo apt install build-essential cmake ninja-build pkg-config qt6-base-dev libmlt++-dev libmlt-dev libavformat-dev libavcodec-dev libswresample-dev libswscale-dev libavutil-dev librubberband-dev libminiaudio-dev
cmake -S . -B build -G Ninja && cmake --build build && ./build/schneidi
```

AppImage: `packaging/linux/build-appimage.sh` → `dist/schneidi-<version>-x86_64.AppImage`
Debian package (system Qt/MLT, no bundling): `cmake --build build && cpack --config build/CPackConfig.cmake -G DEB` → `dist/schneidi_<version>_amd64.deb`
The .deb needs Debian 13 / Ubuntu 25.04 or newer (MLT ≥ 7.30, Qt ≥ 6.8, libc6 ≥ 2.38, libstdc++6 ≥ 14); on older systems use the AppImage.

**Windows:** via [MSYS2](https://www.msys2.org), UCRT64 environment. Clone the repo into a path without spaces or non-ASCII characters, then:

```bash
pacman -S --needed mingw-w64-ucrt-x86_64-{toolchain,cmake,ninja,pkgconf,qt6-base,qt6-svg,mlt,ffmpeg,frei0r-plugins,SDL2,ntldd,libebur128,fftw,libsamplerate,rubberband,rtaudio,miniaudio}
packaging/windows/build-windows.sh
```

Result: `dist/schneidi-windows-<version>.zip` (portable folder) and, if [Inno Setup 6](https://jrsoftware.org/isinfo.php) is installed (`winget install JRSoftware.InnoSetup`), `dist/schneidi-setup-<version>.exe`.

**Tests:**

```bash
cmake -S . -B build-tests -G Ninja -DSCHNEIDI_TESTS=ON && cmake --build build-tests && ctest --test-dir build-tests --output-on-failure
```

## Credits

Developed by [OnkelTheorie](https://github.com/OnkelTheorie) with the help of [Claude](https://claude.ai) (Anthropic) as an AI coding assistant.

Sponsored by [@ZenITDigital](https://github.com/ZenITDigital), who pays for the Claude Max plan behind schneidi's development – thank you so much!

## Disclaimer

schneidi is an independent open-source project. It is not affiliated with, endorsed by or sponsored by Blackmagic Design. DaVinci Resolve is a trademark of Blackmagic Design Pty Ltd and is mentioned here for descriptive purposes only.

## License

[GPL-3.0](LICENSE), with an [additional permission](LICENSE-EXCEPTION.md) for app store libraries (ads, in-app purchases) in schneidi's own code. The code is open; the name "schneidi" and the logo are protected – see [TRADEMARKS.md](TRADEMARKS.md).
