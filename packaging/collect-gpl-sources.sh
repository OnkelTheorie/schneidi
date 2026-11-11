#!/bin/sh
# Gathers the complete corresponding source of the GPL/LGPL parts the packages of one release bundle (GPL-2.0 §3,
# LGPL-2.1 §6) into <out-dir>/schneidi-<version>-gpl-sources/ and packs it as <out-dir>/schneidi-<version>-gpl-sources.tar,
# the release asset that the packages' licenses/SOURCE-OFFER.txt points to (packaging/SOURCE-OFFER.txt.in).
#   packaging/collect-gpl-sources.sh <out-dir> [versions-file …]
# Contents:
#   ffmpeg/   the FFmpeg source release the packages are built from (version and SHA-256 of
#             packaging/linux/build-ffmpeg-lgpl.sh) and that script with packaging/ffmpeg-components.txt (it patches
#             configure and picks the options: the "scripts used to control compilation and installation")
#   debian/   the Debian source packages of x264, x265 and libnuma (numactl) of the AppImage and the Docker image,
#             at the versions their build recorded: versions files = lines "<binary> <version> <source> <source
#             version>" (AppImage: usr/share/licenses/schneidi/x264-x265-versions.txt, default
#             build-appimage/AppDir/…; Docker: /usr/share/doc/schneidi/x264-x265-versions.txt in the image)
#   msys2/    the MSYS2 source packages of x264/x265 of the Windows package, when a versions file is the Windows
#             licenses/x264-x265-NOTICE.txt (lines "mingw-w64-ucrt-x86_64-libx264 <version>")
# Debian sources come via `apt-get source --download-only` (needs deb-src lines), else from deb.debian.org's pool, else
# from snapshot.debian.org (any version Debian ever had). Needs curl, sha256sum, tar; python3 only for the snapshot.
# Runs on Linux (POSIX sh); the Windows parts are plain downloads, so they are gathered here too.
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
[ $# -ge 1 ] || { echo "usage: $0 <out-dir> [versions-file …]" >&2; exit 2; }
OUT_BASE="$1"
shift
[ $# -gt 0 ] || set -- "$ROOT/build-appimage/AppDir/usr/share/licenses/schneidi/x264-x265-versions.txt"

VERSION="$(sed -n 's/^project(schneidi VERSION \([0-9.]*\).*/\1/p' "$ROOT/CMakeLists.txt")"
NAME="schneidi-$VERSION-gpl-sources"
OUT="$OUT_BASE/$NAME"
mkdir -p "$OUT/ffmpeg" "$OUT/debian" "$OUT/msys2"
OUT="$(cd "$OUT" && pwd)"

download() { # url file -> 0 if downloaded
    curl -fsSL --retry 3 -o "$2.part" "$1" 2>/dev/null && mv "$2.part" "$2" && return 0
    rm -f "$2.part"
    return 1
}

# --- FFmpeg: exactly what build-ffmpeg-lgpl.sh downloads ---
BUILD_SCRIPT="$ROOT/packaging/linux/build-ffmpeg-lgpl.sh"
FFMPEG_VERSION="$(sed -n 's/^FFMPEG_VERSION="${FFMPEG_VERSION:-\([^}]*\)}"$/\1/p' "$BUILD_SCRIPT")"
FFMPEG_SHA256="$(sed -n 's/^FFMPEG_SHA256="${FFMPEG_SHA256:-\([^}]*\)}"$/\1/p' "$BUILD_SCRIPT")"
[ -n "$FFMPEG_VERSION" ] && [ -n "$FFMPEG_SHA256" ] || { echo "!! no FFmpeg version in $BUILD_SCRIPT" >&2; exit 1; }
TARBALL="ffmpeg-$FFMPEG_VERSION.tar.xz"
if [ ! -f "$OUT/ffmpeg/$TARBALL" ]; then
    # the build's download, if there is one
    for d in "$ROOT/build-ffmpeg/download" "$ROOT/build-ffmpeg-lgpl/download"; do
        [ -f "$d/$TARBALL" ] && cp "$d/$TARBALL" "$OUT/ffmpeg/" && break
    done
fi
[ -f "$OUT/ffmpeg/$TARBALL" ] || download "https://ffmpeg.org/releases/$TARBALL" "$OUT/ffmpeg/$TARBALL" ||
    { echo "!! could not download $TARBALL" >&2; exit 1; }
echo "$FFMPEG_SHA256  $OUT/ffmpeg/$TARBALL" | sha256sum -c - >/dev/null || { echo "!! $TARBALL: wrong checksum" >&2; exit 1; }
cp "$BUILD_SCRIPT" "$OUT/ffmpeg/build-ffmpeg-lgpl.sh"
cp "$ROOT/packaging/windows/build-ffmpeg-lgpl.sh" "$OUT/ffmpeg/build-ffmpeg-lgpl-windows.sh"
cp "$ROOT/packaging/ffmpeg-components.txt" "$OUT/ffmpeg/"
cp "$ROOT/packaging/linux/Dockerfile.ffmpeg-lgpl" "$OUT/ffmpeg/"
cat >"$OUT/ffmpeg/README.txt" <<EOF
FFmpeg $FFMPEG_VERSION as bundled by schneidi $VERSION (AppImage, Windows, Docker).
$TARBALL is the unchanged release of https://ffmpeg.org/releases/ (SHA-256 $FFMPEG_SHA256).
build-ffmpeg-lgpl.sh unpacks it, patches configure (sed, see the script) and configures it with
--disable-everything plus one --enable-<type>=<name> per line of ffmpeg-components.txt; GPL_ENCODERS=1 (default)
adds --enable-gpl --disable-postproc --enable-libx264 --enable-libx265. The Windows package runs the same script in
MSYS2 UCRT64 (build-ffmpeg-lgpl-windows.sh checks its packages and calls it). Build environment (Debian 13):
Dockerfile.ffmpeg-lgpl.
EOF

# --- Debian source packages ---
debian_source() { # source-package version
    src="$1"
    ver="$2"
    noepoch="${ver#*:}"
    dir="$OUT/debian/${src}_$noepoch"
    if ls "$dir"/*.dsc >/dev/null 2>&1; then return 0; fi
    mkdir -p "$dir"
    echo ">> Debian source $src $ver"
    if command -v apt-get >/dev/null 2>&1 && (cd "$dir" && apt-get source --download-only "$src=$ver" >/dev/null 2>&1); then
        return 0
    fi
    # pool of the current release: the .dsc names the other files
    case "$src" in lib*) pool="$(printf '%s' "$src" | cut -c1-4)" ;; *) pool="$(printf '%s' "$src" | cut -c1)" ;; esac
    base="https://deb.debian.org/debian/pool/main/$pool/$src"
    dsc="${src}_$noepoch.dsc"
    if download "$base/$dsc" "$dir/$dsc"; then
        ok=1
        for f in $(awk '/^Files:/ { f = 1; next } /^[^ ]/ { f = 0 } f && NF == 3 { print $3 }' "$dir/$dsc"); do
            download "$base/$f" "$dir/$f" || ok=0
        done
        [ "$ok" = 1 ] && return 0
    fi
    # snapshot.debian.org: every version Debian ever had
    command -v python3 >/dev/null 2>&1 || { echo "!! $src $ver: not in the pool, snapshot needs python3" >&2; return 1; }
    list="$(curl -fsSL "https://snapshot.debian.org/mr/package/$src/$ver/srcfiles?fileinfo=1" | python3 -I -c '
import json, sys
d = json.load(sys.stdin)
for h, infos in d["fileinfo"].items():
    print(h, infos[0]["name"])
')" || { echo "!! $src $ver: not found on snapshot.debian.org" >&2; return 1; }
    printf '%s\n' "$list" | while read -r hash file; do
        download "https://snapshot.debian.org/file/$hash" "$dir/$file" || { echo "!! $file" >&2; exit 1; }
        # (snapshot.debian.org names files by their SHA-1)
        [ "$(sha1sum "$dir/$file" | cut -d' ' -f1)" = "$hash" ] || { echo "!! $file: wrong checksum" >&2; exit 1; }
    done
}

# --- MSYS2 source packages (Windows) ---
msys2_source() { # package (mingw-w64-ucrt-x86_64-libx264) version (0.165.r3222.b35605a-3, maybe with epoch)
    base="mingw-w64-${1#mingw-w64-ucrt-x86_64-}"
    # the source package is named after the pkgbase: libx264 is built by mingw-w64-x264
    [ "$base" = mingw-w64-libx264 ] && base=mingw-w64-x264
    ver="${2#*:}"
    file="$base-$ver.src.tar.zst"
    [ -f "$OUT/msys2/$file" ] && return 0
    echo ">> MSYS2 source $file"
    download "https://repo.msys2.org/mingw/sources/$file" "$OUT/msys2/$file" ||
        { echo "!! $file not on repo.msys2.org/mingw/sources (epoch in the name? see docs)" >&2; return 1; }
}

FAILED=0
for versions in "$@"; do
    [ -f "$versions" ] || { echo "!! no versions file $versions (build the AppImage first)" >&2; exit 1; }
    cp "$versions" "$OUT/versions-$(basename "$(dirname "$versions")")-$(basename "$versions")"
    # Windows notice: "mingw-w64-ucrt-x86_64-libx264 0.165…-3"; Debian: "<binary> <version> <source> <source version>"
    while read -r a b c d; do
        case "$b" in [0-9]*) ;; *) continue ;; esac # versions only (the notice's other lines)
        case "$a" in
        mingw-w64-ucrt-x86_64-*) [ -n "$b" ] && { msys2_source "$a" "$b" || FAILED=1; } ;;
        *) [ -n "$d" ] && { debian_source "$c" "$d" || FAILED=1; } ;;
        esac
    done <"$versions"
done
[ "$FAILED" = 0 ] || { echo "!! not every source could be gathered (see above)" >&2; exit 1; }

# --- pack ---
(cd "$OUT" && find . -type f ! -name SHA256SUMS | LC_ALL=C sort | sed 's|^\./||' | while read -r f; do sha256sum "$f"; done >SHA256SUMS)
tar -C "$OUT_BASE" -cf "$OUT_BASE/$NAME.tar" "$NAME"
echo ">> done: $OUT_BASE/$NAME.tar ($(du -h "$OUT_BASE/$NAME.tar" | cut -f1))"
