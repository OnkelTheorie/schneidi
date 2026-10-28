#!/usr/bin/env bash
# Builds FFmpeg without any GPL part (LGPL-2.1+ only) for the bundled packages (AppImage, Docker): no x264/x265,
# no GPL-only filters, no rubberband/vidstab/frei0r. Software H.264 comes from OpenH264 (BSD-2), H.265 from kvazaar
# (BSD-3), AV1 from SVT-AV1 (BSD), VP9 from libvpx (BSD); hardware encoders (VAAPI, NVENC, Quick Sync) stay.
#   packaging/linux/build-ffmpeg-lgpl.sh [prefix]          (default prefix: build-ffmpeg-lgpl/prefix)
# Build packages (Debian 13): see packaging/linux/Dockerfile.ffmpeg-lgpl. Then:
#   PKG_CONFIG_PATH=<prefix>/lib/pkgconfig LD_LIBRARY_PATH=<prefix>/lib …   (build-appimage.sh does it with SCHNEIDI_NO_GPL=1)
# The version matches Debian 13's FFmpeg (same library sonames as the system MLT was built against).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
FFMPEG_VERSION="${FFMPEG_VERSION:-7.1.5}"
FFMPEG_SHA256="${FFMPEG_SHA256:-de668509caf9e35e3cd162473441fdb29538c6d96ed080292b3cf9e6fc5d558f}"
NVHEADERS_VERSION="12.2.72.0" # NVENC/NVDEC headers (MIT), header-only
NVHEADERS_SHA256="c295a2ba8a06434d4bdc5c2208f8a825285210d71d91d572329b2c51fd0d4d03"
WORK="${FFMPEG_WORK:-$ROOT/build-ffmpeg-lgpl}"
PREFIX="$(realpath -m "${1:-$WORK/prefix}")"
JOBS="${JOBS:-4}"

mkdir -p "$WORK/download" "$PREFIX"
fetch() { # file url sha256
    local file="$WORK/download/$1"
    if [ ! -f "$file" ]; then
        echo ">> downloading $1"
        curl -fL --retry 3 -o "$file.part" "$2"
        mv "$file.part" "$file"
    fi
    if [[ "$3" != @* ]]; then echo "$3  $file" | sha256sum -c - >/dev/null || { echo "!! checksum of $1 wrong" >&2; exit 1; }
    else echo ">> sha256 of $1 (pin it in this script): $(sha256sum "$file" | cut -d' ' -f1)"; fi
}

# --- NVENC headers (Debian 13 has no package for them in main) ---
if ! pkg-config --exists ffnvcodec 2>/dev/null; then
    fetch "nv-codec-headers-$NVHEADERS_VERSION.tar.gz" \
        "https://github.com/FFmpeg/nv-codec-headers/releases/download/n$NVHEADERS_VERSION/nv-codec-headers-$NVHEADERS_VERSION.tar.gz" \
        "$NVHEADERS_SHA256"
    rm -rf "$WORK/nv-codec-headers"
    mkdir -p "$WORK/nv-codec-headers"
    tar -xzf "$WORK/download/nv-codec-headers-$NVHEADERS_VERSION.tar.gz" -C "$WORK/nv-codec-headers" --strip-components=1
    make -C "$WORK/nv-codec-headers" PREFIX="$PREFIX" install >/dev/null
fi
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig:$PREFIX/share/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"

# --- FFmpeg ---
fetch "ffmpeg-$FFMPEG_VERSION.tar.xz" "https://ffmpeg.org/releases/ffmpeg-$FFMPEG_VERSION.tar.xz" "$FFMPEG_SHA256"
SRC="$WORK/ffmpeg-$FFMPEG_VERSION"
rm -rf "$SRC"
mkdir -p "$SRC"
tar -xJf "$WORK/download/ffmpeg-$FFMPEG_VERSION.tar.xz" -C "$SRC" --strip-components=1

# Optional libraries: only those that are LGPL-compatible (BSD/MIT/LGPL). --enable-gpl and --enable-nonfree stay
# off, so configure itself refuses every GPL-only part (x264, x265, rubberband, vidstab, frei0r, eq, signalstats …).
# --enable-version3 is not needed (no Apache/LGPL-3 library), so the result is LGPL-2.1-or-later.
OPTS=(
    --prefix="$PREFIX" --libdir="$PREFIX/lib" --enable-shared --disable-static
    --extra-version=schneidi-lgpl --disable-doc --disable-ffplay --disable-debug
    --disable-gpl --disable-nonfree
    --enable-libopenh264 --enable-libkvazaar --enable-libsvtav1 --enable-libvpx --enable-libdav1d
    --enable-libopus --enable-libvorbis --enable-libmp3lame --enable-libwebp
    --enable-vaapi --enable-libdrm --enable-libvpl --enable-ffnvcodec --enable-nvenc --enable-nvdec
)
(
    cd "$SRC"
    ./configure "${OPTS[@]}" >"$WORK/configure.log" 2>&1 || { tail -30 "$WORK/configure.log" >&2; exit 1; }
    make -j"$JOBS" >"$WORK/make.log" 2>&1 || { tail -40 "$WORK/make.log" >&2; exit 1; }
    make install >/dev/null
)

# --- Check: nothing GPL in the result (`ffmpeg -L` prints the license the build ended up with) ---
if ! LD_LIBRARY_PATH="$PREFIX/lib" "$PREFIX/bin/ffmpeg" -hide_banner -L 2>&1 | grep -q "Lesser General Public License"; then
    echo "!! FFmpeg is not LGPL:" >&2
    LD_LIBRARY_PATH="$PREFIX/lib" "$PREFIX/bin/ffmpeg" -hide_banner -L 2>&1 | head -5 >&2
    exit 1
fi
echo ">> done: $PREFIX ($(LD_LIBRARY_PATH="$PREFIX/lib" "$PREFIX/bin/ffmpeg" -hide_banner -version | head -1))"
