# Third-party software in the schneidi packages

schneidi itself is AGPL-3.0 (see [LICENSE](LICENSE)). The packages bundle these libraries; each keeps its own license.
All are linked dynamically (shared libraries / DLLs next to the program), so they can be replaced, except RNNoise,
which is compiled into schneidi from `third_party/rnnoise` (its license text: `third_party/rnnoise/COPYING`).
The running program lists what it really loaded: `schneidi-cli --licenses` (AppImage: `schneidi-….AppImage cli --licenses`).

## GPL-free packages (AppImage `packaging/linux/build-appimage.sh`, Docker image)

| Part | License | What for |
|---|---|---|
| [Qt 6](https://www.qt.io) (Core, Gui, Widgets, Network, Svg, DBus; platform plugins) | LGPL-3.0 | user interface |
| [FFmpeg 7.1](https://ffmpeg.org) libraries and the `ffmpeg`/`ffprobe` programs, built by `packaging/linux/build-ffmpeg-lgpl.sh` without `--enable-gpl`/`--enable-nonfree` | LGPL-2.1+ | reading and writing media |
| [OpenH264](https://www.openh264.org) | BSD-2-Clause | H.264 software encoder (see the note below) |
| [kvazaar](https://github.com/ultravideo/kvazaar) (with [Crypto++](https://cryptopp.com), Boost Software License 1.0, which Debian's kvazaar links) | BSD-3-Clause | H.265 software encoder |
| [SVT-AV1](https://gitlab.com/AOMediaCodec/SVT-AV1) | BSD-3-Clause-Clear + AOMedia Patent License 1.0 | AV1 encoder |
| [libvpx](https://chromium.googlesource.com/webm/libvpx) | BSD-3-Clause | VP9 encoder/decoder |
| [dav1d](https://code.videolan.org/videolan/dav1d) | BSD-2-Clause | AV1 decoder |
| [Opus](https://opus-codec.org) | BSD-3-Clause | audio codec |
| [LAME](https://lame.sourceforge.io) | LGPL-2.0+ | MP3 encoder |
| [libva](https://github.com/intel/libva), [libdrm](https://gitlab.freedesktop.org/mesa/drm), [oneVPL](https://github.com/intel/libvpl), [nv-codec-headers](https://github.com/FFmpeg/nv-codec-headers) | MIT | hardware encoders (VAAPI, Quick Sync, NVENC) |
| [miniaudio](https://miniaud.io) | MIT-0 / public domain | sound output |
| [RNNoise](https://gitlab.xiph.org/xiph/rnnoise) v0.2 with its model `0b50c45` (Xiph.Org, Mozilla, Amazon, Jean-Marc Valin; sources in `third_party/rnnoise`, built into the program) | BSD-3-Clause | noise suppression of the effect "Stimme säubern" |
| [Swishes Sound Pack](https://opengameart.org/content/swishes-sound-pack) by artisticdude (8 of its swishes, `assets/sfx/cc0/swish`, built into the program; sources and SHA-256 in `assets/sfx/cc0/SOURCES.md`) | CC0-1.0 | the recorded whoosh and swish sounds of op `sfx` |
| [Impact Sounds](https://kenney.nl/assets/impact-sounds) by Kenney (www.kenney.nl) (5 punches, `assets/sfx/cc0/hit`, built into the program) | CC0-1.0 | the recorded hit sounds of op `sfx` |
| Impact recordings by FFeller and dorian.mastin (Freesound, taken from the [Godot demo projects](https://github.com/godotengine/godot-demo-projects) with their licence files; `assets/sfx/cc0/impact`, built in) | CC0-1.0 | recorded impact sounds of op `sfx` |
| Sounds of Kenney's [Starter Kit Racing](https://github.com/KenneyNL/Starter-Kit-Racing) (impact) and [Starter Kit City Builder](https://github.com/KenneyNL/Starter-Kit-City-Builder) (4 placement clicks) by Kenney (www.kenney.nl) (`assets/sfx/cc0/impact`, `assets/sfx/cc0/click`, built in) | CC0-1.0 | recorded impact and click sounds of op `sfx` |
| [earshot](https://github.com/Leonxlnx/earshot) sound recipes (Copyright (c) 2026 Leon Lin): the whoosh pass engine, riser (the designed build), darkriser, shutter, knock, subdrop and the `_tonal` / `dsp` helpers, with parameter sets of its forge_v2 / forge04 banks, ported to C++ in `src/core/SfxWhoosh.cpp`, `src/core/SfxForge.cpp`, `src/core/SfxDsp.h` (license text: `third_party/earshot/LICENSE`) | MIT | the built-in effect sounds of op `sfx` (whoosh, swish, riser, hit, impact, click) |
| libglvnd (EGL/OpenGL dispatch, fallback only) | MIT-style | systems without a desktop |
| System libraries pulled in by the above (glib, ICU, fontconfig, FreeType, HarfBuzz, libpng, zlib, zstd, libjpeg-turbo, PCRE2, X11/XCB/xkbcommon, Wayland, D-Bus, double-conversion, brotli, md4c …) | LGPL-2.1+, MIT, BSD, Zlib, IJG, Unicode, FTL, AFL-2.1 (D-Bus, dual-licensed) | as used by Qt |

Not in these packages: x264, x265, Rubber Band, vid.stab, frei0r plugins.

whisper.cpp (speech recognition, MIT) is not bundled: schneidi downloads it as an extension on request.

Local AI models are not bundled either; on request (Extensions, `schneidi-cli extensions --install faces`) schneidi
downloads, each pinned by size and SHA-256:

| Extension | License | What for |
|---|---|---|
| [ONNX Runtime](https://github.com/microsoft/onnxruntime) 1.20.1, the CPU build of its GitHub release (Linux x64, Windows x64), id `onnxruntime` | MIT (its `LICENSE` and `ThirdPartyNotices.txt` are kept beside the library) | runs the models below; loaded at run time (QLibrary), never linked |
| [YuNet](https://github.com/opencv/opencv_zoo/tree/48b10fa505aeb51fbd127fc60a5c660040e823b7/models/face_detection_yunet) `face_detection_yunet_2023mar.onnx` (Copyright (c) 2020 Shiqi Yu; opencv_zoo commit `48b10fa`), id `faces` | MIT (its `LICENSE` of the same commit is downloaded beside the model) | faces per shot in the material index (`index`) |

x264/x265 (GPL-2.0+) are not bundled either: on request (Extensions, `schneidi-cli extensions --install x264`)
schneidi downloads an FFmpeg program built with them ([BtbN/FFmpeg-Builds](https://github.com/BtbN/FFmpeg-Builds),
GPL) and runs it as a separate program for H.264/H.265 export when no graphics card encoder works. schneidi does not
link it; it hands it pictures through a pipe and reads back the encoded video.

**OpenH264 and patents:** the OpenH264 code is BSD-licensed, but H.264 is patent-encumbered. Cisco pays the MPEG LA
royalties only for its own prebuilt binary downloaded from Cisco; a self-built or bundled OpenH264 is not covered.
Hardware encoders (the graphics card's driver) are the alternative.

## Packages with the system's FFmpeg (Windows, Debian package; AppImage with `SCHNEIDI_NO_GPL=0`, not released)

As above, but with the FFmpeg of Debian 13 / MSYS2, which is built with `--enable-gpl` (x264, x265, Rubber Band,
vid.stab, frei0r, …) and is therefore GPL-2.0+ as a whole. The Debian package uses these libraries from the system.
Such an AppImage also carries the Qt image format plugins of the build system (KDE's kimageformats: AVIF, HEIF,
JPEG XL, camera RAW, OpenEXR …, LGPL) with their libraries (libavif, libheif, libjxl, LibRaw, OpenEXR; BSD/LGPL).
