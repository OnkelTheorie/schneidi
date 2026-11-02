#!/usr/bin/env bash
# Builds the slim GPL-free FFmpeg (LGPL-2.1+) for the Windows package in MSYS2 UCRT64: the same build as for the
# AppImage (packaging/linux/build-ffmpeg-lgpl.sh, components: packaging/ffmpeg-components.txt), with D3D11VA/DXVA2
# instead of VAAPI. build-windows.sh calls it when its FFMPEG_PREFIX has no ffmpeg.exe yet.
#   packaging/windows/build-ffmpeg-lgpl.sh [prefix]     (default prefix: build-ffmpeg-lgpl/prefix)
# The libraries come from MSYS2 and are all BSD/MIT/LGPL (pacman -Qi … | grep Licenses):
#   openh264 BSD-2, kvazaar BSD-3 (+ crypto++ BSL/public domain), svt-av1 BSD-3-Clear, libvpx BSD-3, dav1d BSD-2,
#   opus BSD-3, lame LGPL (+ mpg123 LGPL-2.1), libvpl MIT, ffnvcodec-headers MIT; nasm/make/diffutils only build it.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
[ "${MSYSTEM:-}" = "UCRT64" ] || { echo "!! Please run in the MSYS2 environment UCRT64" >&2; exit 1; }

PACKAGES=(nasm make diffutils)
for p in openh264 kvazaar svt-av1 libvpx dav1d opus lame libvpl ffnvcodec-headers; do
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
