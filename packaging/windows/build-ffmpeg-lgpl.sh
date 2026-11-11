#!/usr/bin/env bash
# Builds the slim FFmpeg for the Windows package in MSYS2 UCRT64: the same build as for the AppImage
# (packaging/linux/build-ffmpeg-lgpl.sh, components: packaging/ffmpeg-components.txt), with D3D11VA/DXVA2 instead of
# VAAPI. build-windows.sh calls it when its FFMPEG_PREFIX has no ffmpeg.exe yet.
#   packaging/windows/build-ffmpeg-lgpl.sh [prefix]
# GPL_ENCODERS=1 (default, the GitHub download since 0.21): with x264/x265 (GPL-2.0+), prefix build-ffmpeg/prefix.
# GPL_ENCODERS=0: GPL-free (LGPL-2.1+), prefix build-ffmpeg-lgpl/prefix.
# The libraries come from MSYS2 (pacman -Qi … | grep Licenses):
#   openh264 BSD-2, kvazaar BSD-3 (+ crypto++ BSL/public domain), svt-av1 BSD-3-Clear, libvpx BSD-3, dav1d BSD-2,
#   opus BSD-3, lame LGPL (+ mpg123 LGPL-2.1), libvpl MIT, ffnvcodec-headers MIT; only with GPL_ENCODERS=1:
#   x264 GPL-2+, x265 GPL-2+; nasm/make/diffutils only build it.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
[ "${MSYSTEM:-}" = "UCRT64" ] || { echo "!! Please run in the MSYS2 environment UCRT64" >&2; exit 1; }

PACKAGES=(nasm make diffutils)
LIBS=(openh264 kvazaar svt-av1 libvpx dav1d opus lame libvpl ffnvcodec-headers)
[ "${GPL_ENCODERS:-1}" = 1 ] && LIBS+=(x264 x265)
for p in "${LIBS[@]}"; do
    PACKAGES+=("mingw-w64-ucrt-x86_64-$p")
done
MISSING=()
for p in "${PACKAGES[@]}"; do pacman -Q "$p" >/dev/null 2>&1 || MISSING+=("$p"); done
if [ ${#MISSING[@]} -gt 0 ]; then
    echo "!! missing build packages, install them with:" >&2
    echo "   pacman -S --needed ${MISSING[*]}" >&2
    exit 1
fi

exec "$ROOT/packaging/linux/build-ffmpeg-lgpl.sh" "$@"
