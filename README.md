# schneidi

A lightweight video editor with Media, Edit, Color and Deliver pages. Its workflow is inspired by DaVinci Resolve, with far fewer features. The UI is available in English and German.

[![Download](https://img.shields.io/github/v/release/OnkelTheorie/schneidi?label=Download&color=2ea44f)](https://github.com/OnkelTheorie/schneidi/releases/latest)

**[Download the latest release](https://github.com/OnkelTheorie/schneidi/releases/latest)** (Linux AppImage or Debian package). To build from source, see [Building](#building).

> Status: early development (0.x). The project file format may still change between versions.

![Edit page](screenshots/edit.png)

## Features

- Multiple video and audio tracks: cut, trim, move, snapping, undo/redo
- Timeline zoom and scroll change **only on user input** – nothing jumps when clips get shorter
- Fully customizable keyboard shortcuts
- Titles, subtitles, fades, transitions, volume, loudness normalization
- Color correction, curves, LUTs, effects with keyframes
- frei0r effects; import effect presets from Kdenlive and Shotcut
- Video scopes: waveform, RGB parade, vectorscope, histogram
- Two-up/four-up trim view while dragging an edit
- Loudness and true-peak meters
- Clip speed / retime
- Proxy and render cache for smooth playback
- Export and render queue via FFmpeg
- Themes, English/German

<details>
<summary>Color page</summary>

![Color page](screenshots/color.png)

</details>

## Building

C++20, Qt 6 Widgets, MLT 7, CMake.

**Linux (Debian/Ubuntu):**

```bash
sudo apt install build-essential cmake ninja-build pkg-config qt6-base-dev libmlt++-dev libmlt-dev
cmake -S . -B build -G Ninja && cmake --build build && ./build/schneidi
```

AppImage: `packaging/linux/build-appimage.sh` → `dist/schneidi-<version>-x86_64.AppImage`
Debian package (system Qt/MLT, no bundling): `cmake --build build && cpack --config build/CPackConfig.cmake -G DEB` → `dist/schneidi_<version>_amd64.deb`

**Windows:** via [MSYS2](https://www.msys2.org), UCRT64 environment. Clone the repo into a path without spaces or non-ASCII characters, then:

```bash
pacman -S --needed mingw-w64-ucrt-x86_64-{toolchain,cmake,ninja,pkgconf,qt6-base,qt6-svg,mlt,ffmpeg,frei0r-plugins,SDL2,ntldd,libebur128,fftw,libsamplerate,rubberband,rtaudio}
packaging/windows/build-windows.sh
```

Result: `dist/schneidi-windows-<version>.zip` (portable folder) and, if [Inno Setup 6](https://jrsoftware.org/isinfo.php) is installed (`winget install JRSoftware.InnoSetup`), `dist/schneidi-setup-<version>.exe`.

**Tests:**

```bash
cmake -S . -B build-tests -G Ninja -DSCHNEIDI_TESTS=ON && cmake --build build-tests && ctest --test-dir build-tests --output-on-failure
```

## Credits

Developed by [OnkelTheorie](https://github.com/OnkelTheorie) with the help of [Claude](https://claude.ai) (Anthropic) as an AI coding assistant.

## Disclaimer

schneidi is an independent open-source project. It is not affiliated with, endorsed by or sponsored by Blackmagic Design. DaVinci Resolve is a trademark of Blackmagic Design Pty Ltd and is mentioned here for descriptive purposes only.

## License

[GPL-3.0](LICENSE)
