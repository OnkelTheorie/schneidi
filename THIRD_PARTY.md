# Third-party software in the schneidi packages

schneidi itself is AGPL-3.0 (see [LICENSE](LICENSE)). The packages bundle these libraries; each keeps its own license.
All are linked dynamically (shared libraries / DLLs next to the program), so they can be replaced.
The running program lists what it really loaded: `schneidi-cli --licenses` (AppImage: `schneidi-….AppImage cli --licenses`).

## GPL-free packages (`SCHNEIDI_NO_GPL=1 packaging/linux/build-appimage.sh`)

| Part | License | What for |
|---|---|---|
| [Qt 6](https://www.qt.io) (Core, Gui, Widgets, Network, Svg, DBus; platform plugins) | LGPL-3.0 | user interface |
| [MLT 7](https://www.mltframework.org) framework and the modules core, avformat, sdl2, rtaudio, xml, frei0r (the wrapper only) | LGPL-2.1+ (rtaudio: MIT) | preview and export graph |
| [FFmpeg 7.1](https://ffmpeg.org) libraries and the `ffmpeg`/`ffprobe` programs, built by `packaging/linux/build-ffmpeg-lgpl.sh` without `--enable-gpl`/`--enable-nonfree` | LGPL-2.1+ | reading and writing media |
| [OpenH264](https://www.openh264.org) | BSD-2-Clause | H.264 software encoder (see the note below) |
| [kvazaar](https://github.com/ultravideo/kvazaar) | BSD-3-Clause | H.265 software encoder |
| [SVT-AV1](https://gitlab.com/AOMediaCodec/SVT-AV1) | BSD-3-Clause-Clear + AOMedia Patent License 1.0 | AV1 encoder |
| [libvpx](https://chromium.googlesource.com/webm/libvpx) | BSD-3-Clause | VP9 encoder/decoder |
| [dav1d](https://code.videolan.org/videolan/dav1d) | BSD-2-Clause | AV1 decoder |
| [Opus](https://opus-codec.org), [Vorbis/Ogg](https://xiph.org) | BSD-3-Clause | audio codecs |
| [LAME](https://lame.sourceforge.io) | LGPL-2.0+ | MP3 encoder |
| [libwebp](https://chromium.googlesource.com/webm/libwebp) | BSD-3-Clause | WebP pictures |
| [libva](https://github.com/intel/libva), [libdrm](https://gitlab.freedesktop.org/mesa/drm), [oneVPL](https://github.com/intel/libvpl), [nv-codec-headers](https://github.com/FFmpeg/nv-codec-headers) | MIT | hardware encoders (VAAPI, Quick Sync, NVENC) |
| [SDL 2](https://www.libsdl.org) | Zlib | MLT's sdl2 module |
| [libsamplerate](https://libsndfile.github.io/libsamplerate/) | BSD-2-Clause | MLT's sdl2 module |
| [miniaudio](https://miniaud.io) | MIT-0 / public domain | sound output |
| libglvnd (EGL/OpenGL dispatch, fallback only) | MIT-style | systems without a desktop |
| System libraries pulled in by the above (glib, ICU, fontconfig, FreeType, HarfBuzz, libpng, zlib, zstd, libjpeg-turbo, PCRE2, X11/XCB/xkbcommon, Wayland, D-Bus, double-conversion, brotli, md4c …) | LGPL-2.1+, MIT, BSD, Zlib, IJG, Unicode, FTL, AFL-2.1 (D-Bus, dual-licensed) | as used by Qt |

Not in these packages: x264, x265, Rubber Band, vid.stab, frei0r plugins, MLT's GPL modules (qt, plusgpl, plus,
normalize, resample, xine, vid.stab, rubberband, jackrack, movit, sox). frei0r effects stay possible as an option
(Settings › General › Effects) with plugins the user installs.

whisper.cpp (speech recognition, MIT) is not bundled: schneidi downloads it as an extension on request.

**OpenH264 and patents:** the OpenH264 code is BSD-licensed, but H.264 is patent-encumbered. Cisco pays the MPEG LA
royalties only for its own prebuilt binary downloaded from Cisco; a self-built or bundled OpenH264 is not covered.
Hardware encoders (the graphics card's driver) are the alternative.

## Regular packages (AppImage without `SCHNEIDI_NO_GPL`, Windows, Debian package, Docker)

As above, but with the FFmpeg of Debian 13 / MSYS2, which is built with `--enable-gpl` (x264, x265, Rubber Band,
vid.stab, frei0r, …) and is therefore GPL-2.0+ as a whole; the AppImage and the Windows package also bundle the frei0r
plugins (GPL-2.0+). The Debian package and the Docker image use these libraries from the system.
