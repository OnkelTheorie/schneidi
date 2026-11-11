#!/usr/bin/env bash
# Builds the slim FFmpeg of the bundled packages (AppImage, Windows program folder, Docker image): only the components
# in packaging/ffmpeg-components.txt (what schneidi uses; smaller package, faster start).
#   packaging/linux/build-ffmpeg-lgpl.sh [prefix]
# Two variants (GPL_ENCODERS):
#   1 (default, the GitHub downloads since 0.21): with x264 and x265 for the best software H.264/H.265 quality.
#     FFmpeg is configured with --enable-gpl, so this FFmpeg (and only it: schneidi's own source stays GPL-free) is
#     GPL-2.0-or-later. x264/x265 come from the system (Debian libx264-dev libx265-dev, MSYS2 mingw-w64-ucrt-x86_64-
#     x264/x265) and are linked as shared libraries that the packages carry. Default prefix: build-ffmpeg/prefix.
#   0: without any GPL part (LGPL-2.1+ only), for builds that must stay GPL-free (app stores): no x264/x265, no
#     GPL-only filters, no rubberband/vidstab/frei0r. Software H.264 comes from OpenH264 (BSD-2), H.265 from kvazaar
#     (BSD-3). Default prefix: build-ffmpeg-lgpl/prefix.
# Both: AV1 from SVT-AV1 (BSD), VP9 from libvpx (BSD); hardware encoders (VAAPI, NVENC, Quick Sync) stay.
# FFMPEG_FULL=1 builds everything configure allows (for comparisons: tools/ffmpeg-import-check.sh).
# The licence texts FFmpeg's binaries need (and the GPL-2.0 text) go to <prefix>/share/licenses/ffmpeg.
# Also runs in MSYS2 UCRT64 (Windows program folder; D3D11VA/AMF instead of VAAPI, see packaging/windows).
# Build packages (Debian 13): see packaging/linux/Dockerfile.ffmpeg-lgpl. Then:
#   PKG_CONFIG_PATH=<prefix>/lib/pkgconfig LD_LIBRARY_PATH=<prefix>/lib …   (build-appimage.sh does it)
# The version matches Debian 13's FFmpeg (same library sonames as the system libraries).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
FFMPEG_VERSION="${FFMPEG_VERSION:-7.1.5}"
FFMPEG_SHA256="${FFMPEG_SHA256:-de668509caf9e35e3cd162473441fdb29538c6d96ed080292b3cf9e6fc5d558f}"
NVHEADERS_VERSION="12.2.72.0" # NVENC/NVDEC headers (MIT), header-only
NVHEADERS_SHA256="c295a2ba8a06434d4bdc5c2208f8a825285210d71d91d572329b2c51fd0d4d03"
GPL_ENCODERS="${GPL_ENCODERS:-1}"
case "$GPL_ENCODERS" in 0 | 1) ;; *) echo "!! GPL_ENCODERS must be 0 or 1" >&2; exit 1 ;; esac
if [ "$GPL_ENCODERS" = 1 ]; then WORK="${FFMPEG_WORK:-$ROOT/build-ffmpeg}"; else WORK="${FFMPEG_WORK:-$ROOT/build-ffmpeg-lgpl}"; fi
PREFIX="$(realpath -m "${1:-$WORK/prefix}")"
JOBS="${JOBS:-4}"
FULL="${FFMPEG_FULL:-0}"
COMPONENTS="$ROOT/packaging/ffmpeg-components.txt"
case "$(uname -s)" in MINGW* | MSYS* | CYGWIN*) WINDOWS=1 ;; *) WINDOWS=0 ;; esac

mkdir -p "$WORK/download" "$PREFIX"
fetch() { # file url sha256
    local file="$WORK/download/$1"
    if [ ! -f "$file" ]; then
        echo ">> downloading $1"
        curl -fL --retry 5 --retry-all-errors -o "$file.part" "$2"
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
# The metadata/ametadata filters write their file output with libavformat, but configure (7.1) does not link
# libavfilter against it. Linux resolves that at run time; a Windows DLL must resolve it at link time
# ("undefined reference to avio_open"). Add the missing dependency next to fsync's.
for f in metadata ametadata; do
    grep -q "enabled ${f}_filter " "$SRC/configure" ||
        sed -i "s/^enabled fsync_filter .*/&\nenabled ${f}_filter \&\& prepend avfilter_deps \"avformat\"/" "$SRC/configure"
done

# Optional libraries: BSD/MIT/LGPL ones always. GPL_ENCODERS=0: --enable-gpl and --enable-nonfree stay off, so
# configure itself refuses every GPL-only part (x264, x265, rubberband, vidstab, frei0r, eq, signalstats …);
# --enable-version3 is not needed (no Apache/LGPL-3 library), so the result is LGPL-2.1-or-later.
# GPL_ENCODERS=1: --enable-gpl and x264/x265 on top (configure stops if their libraries are missing); nothing else GPL
# is enabled (the component list decides), --enable-nonfree stays off (the result may be redistributed).
OPTS=(
    --prefix="$PREFIX" --libdir="$PREFIX/lib" --enable-shared --disable-static
    --disable-doc --disable-ffplay --disable-debug --disable-nonfree
    --enable-zlib --enable-bzlib --enable-lzma # PNG/TIFF encoders, compressed MP4/MKV headers: required, not optional
    --enable-libopenh264 --enable-libkvazaar --enable-libsvtav1 --enable-libvpx --enable-libdav1d
    --enable-libopus --enable-libmp3lame
    --enable-libvpl --enable-ffnvcodec --enable-nvenc --enable-nvdec
)
if [ "$GPL_ENCODERS" = 1 ]; then OPTS+=(--extra-version=schneidi --enable-gpl --enable-libx264 --enable-libx265)
else OPTS+=(--extra-version=schneidi-lgpl --disable-gpl); fi
if [ "$WINDOWS" = 1 ]; then OPTS+=(--enable-d3d11va --enable-dxva2)
else OPTS+=(--enable-vaapi --enable-libdrm); fi
if [ "$FULL" = 1 ]; then
    OPTS+=(--enable-libvorbis --enable-libwebp --extra-version=schneidi-full)
else
    # --disable-everything, then one --enable-<type>=<name> per line of the list (devices: only the lavfi input of the
    # hardware encoder test)
    OPTS+=(--disable-everything --disable-autodetect --enable-pthreads --enable-cuda)
    [ "$WINDOWS" = 1 ] && OPTS+=(--enable-w32threads --disable-pthreads)
    known() { (cd "$SRC" && ./configure --list-"$1"s) | tr -s ' \t' '\n'; }
    declare -A LISTS=()
    while read -r type name _; do
        [ -z "$type" ] || [[ "$type" == \#* ]] && continue
        [ -n "${LISTS[$type]+x}" ] || LISTS[$type]="$(known "$type")"
        if ! grep -qx -- "$(printf '%s' "$name" | sed 's/[.]/\\./g; s/[*]/.*/g')" <<<"${LISTS[$type]}"; then
            echo "!! $COMPONENTS: FFmpeg $FFMPEG_VERSION has no $type '$name'" >&2
            exit 1
        fi
        OPTS+=("--enable-$type=$name")
    done <"$COMPONENTS"
fi
(
    cd "$SRC"
    ./configure "${OPTS[@]}" >"$WORK/configure.log" 2>&1 || { tail -30 "$WORK/configure.log" >&2; exit 1; }
    make -j"$JOBS" >"$WORK/make.log" 2>&1 || { tail -40 "$WORK/make.log" >&2; exit 1; }
    make install >/dev/null
)

# --- Licence texts for the packages (FFmpeg's LICENSE.md names the parts; GPL-2.0 for the GPL build: FFmpeg with
# x264/x265 as a whole, x264 and x265 themselves) ---
mkdir -p "$PREFIX/share/licenses/ffmpeg"
cp "$SRC/LICENSE.md" "$SRC/COPYING.LGPLv2.1" "$PREFIX/share/licenses/ffmpeg/"
[ "$GPL_ENCODERS" = 1 ] && cp "$SRC/COPYING.GPLv2" "$PREFIX/share/licenses/ffmpeg/"

# --- Check the licence the build ended up with (`ffmpeg -L`) and, for GPL_ENCODERS=1, that x264/x265 are in ---
LICENSE_TEXT="$(LD_LIBRARY_PATH="$PREFIX/lib" "$PREFIX/bin/ffmpeg" -hide_banner -L 2>&1 | head -12)"
if [ "$GPL_ENCODERS" = 0 ]; then
    if ! grep -q "Lesser General Public License" <<<"$LICENSE_TEXT"; then
        echo "!! FFmpeg is not LGPL:" >&2
        echo "$LICENSE_TEXT" | head -5 >&2
        exit 1
    fi
else
    if grep -qi "nonfree\|unredistributable" <<<"$LICENSE_TEXT"; then
        echo "!! FFmpeg is nonfree (may not be redistributed):" >&2
        echo "$LICENSE_TEXT" | head -5 >&2
        exit 1
    fi
    ENCODERS="$(LD_LIBRARY_PATH="$PREFIX/lib" "$PREFIX/bin/ffmpeg" -hide_banner -encoders 2>/dev/null)"
    for e in libx264 libx265; do
        grep -qE "^ *V[^ ]* +$e " <<<"$ENCODERS" || { echo "!! the GPL_ENCODERS=1 build has no $e encoder" >&2; exit 1; }
    done
fi
echo ">> done: $PREFIX ($(LD_LIBRARY_PATH="$PREFIX/lib" "$PREFIX/bin/ffmpeg" -hide_banner -version | head -1))"
