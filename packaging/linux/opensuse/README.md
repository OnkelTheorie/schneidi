# Packaging schneidi for a Linux distribution

`schneidi.spec` is a reference spec file for openSUSE (Tumbleweed; the Open Build Service). It is tested with
`rpmbuild` in an `opensuse/tumbleweed` container, without network, from a `git archive` tarball. Other distributions
need the same steps.

## What the build needs

- CMake ≥ 3.21, a C++20 compiler (GCC or Clang), Ninja (or Make)
- Qt 6 (tested: 6.8 on Debian 13, 6.12 on Tumbleweed): Widgets, Network, DBus (optional: "show in file manager"),
  Test (only for the tests). The GPU viewer uses Qt's private RHI headers (openSUSE `qt6-gui-private-devel`, Debian
  `qt6-base-private-dev`); without them it is built with the QPainter viewer only (`-DSCHNEIDI_RHI=OFF` turns it
  off on purpose).
- FFmpeg libraries: libavformat, libavcodec, libswresample, libswscale, libavutil (tested: FFmpeg 7.1 on Debian 13,
  9.0 on Tumbleweed)
- miniaudio (a single header): the system's `miniaudio.h` when there is one (Debian `libminiaudio-dev`, MSYS2),
  otherwise the unchanged copy of the 0.11.22 release in `third_party/miniaudio` (SHA-256 in its `VERSION.md`).
  `-DSCHNEIDI_SYSTEM_MINIAUDIO=ON` insists on the system's, `OFF` always takes the copy.
- The RNNoise model `rnnoise_data-0b50c45.tar.gz` (22 MB, from `https://media.xiph.org/rnnoise/models/`). The build
  downloads it when it is missing; for a build without network put it into `third_party/rnnoise/` (as the spec does
  with `Source1`) or give `-DSCHNEIDI_RNNOISE_MODEL=<file>`. Its SHA-256 is checked.

No MLT, no frei0r, no SDL: schneidi has its own picture and sound engines on the FFmpeg libraries since 0.15.

## What gets installed

| File | What |
|---|---|
| `bin/schneidi` | the program |
| `bin/schneidi-cli` | the command line program (also the MCP server: `schneidi-cli mcp`) |
| `share/applications/schneidi.desktop` | desktop entry |
| `share/icons/hicolor/scalable/apps/schneidi.svg` | icon |
| `share/metainfo/io.github.OnkelTheorie.schneidi.metainfo.xml` | AppStream metadata (`launchable` = `schneidi.desktop`) |
| `share/doc/schneidi/copyright`, `changelog.gz` | Debian policy files; the spec removes them and packages the license files with `%license` |

From a source tarball (no `.git` folder) the programs are linked statically against schneidi's own code; a git
checkout links shared development libraries instead (`SCHNEIDI_SHARED_LIBS`). Release builds are always static.

## Tests

`-DSCHNEIDI_TESTS=ON` builds the test programs; `ctest` runs them (`QT_QPA_PLATFORM=offscreen`, no display needed).
Tests that need a program that is missing (`ffmpeg`, `ffprobe`) skip themselves. A few tests measure time and can
fail on a loaded build machine; `tools/run_tests.py <build folder>` reruns failures one by one.

The tests make their test videos with libx264, so they need an FFmpeg with it. openSUSE's own FFmpeg has no H.264 or
H.265 encoder (Packman's has): about half of the tests fail there, which is why the spec runs them only with
`--with tests` and otherwise checks that `schneidi-cli --version` starts. schneidi itself exports H.264/H.265 with the
graphics card or OpenH264/kvazaar when the FFmpeg has them, and offers x264/x265 as an optional download on x86_64.

Tested on 32-bit x86 as well (Debian 13 i386, x87 floating point): everything passes except one colour comparison of
`test_analysis_scan`. Debian's i386 x264 has no assembly, so `test_short` takes about 10 minutes there, longer than its
5-minute timeout.

## Licenses

schneidi is AGPL-3.0-only. Compiled in: RNNoise (BSD-3-Clause), the earshot sound recipes (MIT), recorded sounds
(CC0-1.0) and miniaudio (MIT-0 or Unlicense). See `THIRD_PARTY.md`.

## Runtime

miniaudio opens ALSA or PulseAudio/PipeWire at run time (`dlopen`), so automatic dependency scanners do not see them:
require `libasound2`, recommend the PulseAudio client library. The `ffmpeg` program is recommended (the previews of
the Media Storage are made by it). Speech recognition (whisper.cpp), text to speech and x264/x265 are optional
downloads inside the program, not build dependencies.
