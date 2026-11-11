#!/usr/bin/env bash
# Builds dist/schneidi-<version>-x86_64.AppImage (release build + Qt, the own slim FFmpeg with ffmpeg/ffprobe).
# GPL_ENCODERS (the FFmpeg of packaging/linux/build-ffmpeg-lgpl.sh, FFMPEG_PREFIX, built there if missing):
#   1 (default, the GitHub download since 0.21): FFmpeg with x264/x265 (GPL-2.0+; build packages libx264-dev
#     libx265-dev). schneidi's own code is the same as in the GPL-free build; the package carries libx264/libx265,
#     the GPL-2.0 text and their notices (usr/share/licenses).
#   0: nothing GPL in the package (SCHNEIDI_NO_GPL: OpenH264/kvazaar instead of x264/x265, which stay an optional
#     download in the program, engine/Extensions "x264") -> …-x86_64-lgpl.AppImage, checked with
#     `schneidi-cli --licenses`. SCHNEIDI_NO_GPL=1 in the environment means the same.
# AppDir layout like the Windows program folder (see src/engine/Bundle.h):
#   usr/bin/{schneidi,schneidi-cli,ffmpeg,ffprobe}
# Run from the project folder: packaging/linux/build-appimage.sh
# CLI from the AppImage: `schneidi-<version>-x86_64.AppImage cli help` or a link named schneidi-cli (packaging/linux/AppRun)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TOOLS="$ROOT/packaging/linux/tools"
ARCH="$(uname -m)"
MULTIARCH="$(gcc -print-multiarch 2>/dev/null || echo "$ARCH-linux-gnu")"

GPL_ENCODERS="${GPL_ENCODERS:-1}"
[ "${SCHNEIDI_NO_GPL:-}" = 1 ] && GPL_ENCODERS=0
case "$GPL_ENCODERS" in 0 | 1) ;; *) echo "!! GPL_ENCODERS must be 0 or 1" >&2; exit 1 ;; esac
export GPL_ENCODERS # for build-ffmpeg-lgpl.sh
CMAKE_EXTRA=()
SUFFIX=""
if [ "$GPL_ENCODERS" = 1 ]; then
    BUILD="$ROOT/build-appimage"
    FFMPEG_PREFIX="${FFMPEG_PREFIX:-$ROOT/build-ffmpeg/prefix}"
    # schneidi itself is built the same way; only the GPL-free build refuses a GPL FFmpeg
    CMAKE_EXTRA+=(-DSCHNEIDI_NO_GPL=OFF)
else
    BUILD="$ROOT/build-appimage-lgpl"
    FFMPEG_PREFIX="${FFMPEG_PREFIX:-$ROOT/build-ffmpeg-lgpl/prefix}"
    CMAKE_EXTRA+=(-DSCHNEIDI_NO_GPL=ON)
    SUFFIX="-lgpl"
fi
APPDIR="$BUILD/AppDir"
DIST="$ROOT/dist"
[ -x "$FFMPEG_PREFIX/bin/ffmpeg" ] || "$ROOT/packaging/linux/build-ffmpeg-lgpl.sh" "$FFMPEG_PREFIX"
# A prefix of the other variant (FFMPEG_PREFIX given by hand) would give a package that is not what its name says
if [ "$GPL_ENCODERS" = 1 ]; then
    encoders="$(LD_LIBRARY_PATH="$FFMPEG_PREFIX/lib" "$FFMPEG_PREFIX/bin/ffmpeg" -hide_banner -encoders 2>/dev/null || true)"
    grep -q ' libx264 ' <<<"$encoders" || { echo "!! $FFMPEG_PREFIX has no libx264 (built with GPL_ENCODERS=0?)" >&2; exit 1; }
fi
export PKG_CONFIG_PATH="$FFMPEG_PREFIX/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
# linuxdeploy resolves libavcodec … to these (same sonames as Debian's)
export LD_LIBRARY_PATH="$FFMPEG_PREFIX/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
FFMPEG_BIN_DIR="$FFMPEG_PREFIX/bin"

# pkg-config results stay in the CMake cache: another FFmpeg than last time -> configure again from scratch
STAMP="$BUILD/ffmpeg-prefix.txt"
if [ -f "$BUILD/CMakeCache.txt" ] && [ "$(cat "$STAMP" 2>/dev/null)" != "$FFMPEG_PREFIX" ]; then
    echo ">> other FFmpeg than the last build: configuring $BUILD again"
    rm -f "$BUILD/CMakeCache.txt"
fi
mkdir -p "$BUILD"
printf '%s' "$FFMPEG_PREFIX" >"$STAMP"

# --- Werkzeuge (einmalig herunterladen) ---
mkdir -p "$TOOLS"
fetch() {
    local file="$TOOLS/$1" url="$2"
    if [ ! -x "$file" ]; then
        echo ">> lade $1"
        curl -fL --retry 3 -o "$file.part" "$url"
        mv "$file.part" "$file"
        chmod +x "$file"
    fi
}
fetch "linuxdeploy-$ARCH.AppImage" \
    "https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-$ARCH.AppImage"
fetch "linuxdeploy-plugin-qt-$ARCH.AppImage" \
    "https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-$ARCH.AppImage"
# Ohne FUSE lauffähig
export APPIMAGE_EXTRACT_AND_RUN=1

# --- Release-Build und Installation ins AppDir ---
rm -rf "$APPDIR"
cmake -S "$ROOT" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr "${CMAKE_EXTRA[@]}"
cmake --build "$BUILD"
DESTDIR="$APPDIR" cmake --install "$BUILD"

# --- ffmpeg dazulegen ---
mkdir -p "$APPDIR/usr/bin"
for t in ffmpeg ffprobe; do cp -L "$FFMPEG_BIN_DIR/$t" "$APPDIR/usr/bin/"; done

# Licence texts (like the Windows program folder's licenses/): schneidi, the bundled parts, FFmpeg's; for the GPL
# build also GPL-2.0 and the notices of x264/x265 (the system's copyright files of the libraries that get bundled)
LICENSES="$APPDIR/usr/share/licenses/schneidi"
mkdir -p "$LICENSES"
cp "$ROOT/LICENSE" "$ROOT/THIRD_PARTY.md" "$LICENSES/"
cp "$ROOT/third_party/earshot/LICENSE" "$LICENSES/earshot-LICENSE.txt"
cp "$ROOT/third_party/rnnoise/COPYING" "$LICENSES/rnnoise-COPYING.txt"
cp "$ROOT"/assets/sfx/cc0/LICENSE-* "$ROOT/assets/sfx/cc0/SOURCES.md" "$LICENSES/"
for f in "$FFMPEG_PREFIX"/share/licenses/ffmpeg/*; do cp "$f" "$LICENSES/ffmpeg-$(basename "$f")"; done
if [ "$GPL_ENCODERS" = 1 ]; then
    for lib in x264 x265; do
        so="$(ldd "$FFMPEG_PREFIX/lib/libavcodec.so" | awk -v l="lib$lib.so" '!f && index($1, l) == 1 { print $3; f = 1 }')"
        [ -n "$so" ] || { echo "!! libavcodec does not link lib$lib" >&2; exit 1; }
        real="$(realpath "$so")" # merged /usr: dpkg knows the file under /usr/lib or /lib
        pkg="$({ dpkg -S "$real" || dpkg -S "${real#/usr}"; } 2>/dev/null | head -1 | cut -d: -f1 || true)"
        if [ -n "$pkg" ] && [ -f "/usr/share/doc/$pkg/copyright" ]; then
            cp "/usr/share/doc/$pkg/copyright" "$LICENSES/$lib-copyright.txt"
            echo "$pkg $(dpkg-query -W -f='${Version}\n' "$pkg" 2>/dev/null | head -1)" >>"$LICENSES/x264-x265-versions.txt"
        else
            # GPL-2.0 asks for the notices in every copy: no package without them
            echo "!! no copyright file for $so (not a Debian package?)" >&2
            exit 1
        fi
    done
fi

# GL dispatch libraries (libglvnd) as a fallback for systems without a desktop: AppRun adds a folder to the library
# path only if the system lacks that library (a bundled one could clash with the system's GPU driver)
for lib in libEGL.so.1 libOpenGL.so.0 libGLdispatch.so.0; do
    src="/usr/lib/$MULTIARCH/$lib"
    [ -f "$src" ] || src="$(ldconfig -p | awk -v l="$lib" '$1 == l { print $NF; exit }')"
    if [ -n "$src" ] && [ -f "$src" ]; then
        mkdir -p "$APPDIR/usr/lib/fallback/${lib%%.so*}"
        cp -L "$src" "$APPDIR/usr/lib/fallback/${lib%%.so*}/$lib"
    else
        echo "!! $lib not found (the AppImage will need it on the system)" >&2
    fi
done

# --- Abhängigkeiten + Qt einsammeln, AppImage bauen ---
QMAKE_BIN="$(command -v qmake6 || true)"
[ -n "$QMAKE_BIN" ] || QMAKE_BIN="/usr/lib/qt6/bin/qmake"
export QMAKE="$QMAKE_BIN"
export EXTRA_PLATFORM_PLUGINS="libqwayland-generic.so;libqoffscreen.so" # offscreen: --screenshot-Testlauf
export EXTRA_QT_MODULES="svg"
VERSION="$(sed -n 's/^project(schneidi VERSION \([0-9.]*\).*/\1/p' "$ROOT/CMakeLists.txt")"
export LINUXDEPLOY_OUTPUT_VERSION="${VERSION:-0.1.0}"

# OpenGL under Wayland needs Qt's EGL client integration (the GPU viewer and picture engine); the Qt plugin of
# linuxdeploy brings the Wayland platform but not this one: copied here, its libraries collected below
QT_PLUGINS="$("$QMAKE_BIN" -query QT_INSTALL_PLUGINS)"
WAYLAND_EGL="$QT_PLUGINS/wayland-graphics-integration-client/libqt-plugin-wayland-egl.so"
DEPLOY_EXTRA=()
if [ -f "$WAYLAND_EGL" ]; then
    mkdir -p "$APPDIR/usr/plugins/wayland-graphics-integration-client"
    cp -L "$WAYLAND_EGL" "$APPDIR/usr/plugins/wayland-graphics-integration-client/"
    DEPLOY_EXTRA+=(--deploy-deps-only "$APPDIR/usr/plugins/wayland-graphics-integration-client")
else
    echo "!! $WAYLAND_EGL not found (qt6-wayland): no OpenGL under Wayland" >&2
fi

# linuxdeploy names the file by version only: packed in the build folder, then moved to dist/ under the variant's
# name (so building one variant does not overwrite the other's package)
mkdir -p "$DIST" "$BUILD/out"
rm -f "$BUILD"/out/*.AppImage
cd "$BUILD/out"
"$TOOLS/linuxdeploy-$ARCH.AppImage" --appdir "$APPDIR" "${DEPLOY_EXTRA[@]}" \
    --executable "$APPDIR/usr/bin/schneidi-cli" \
    --executable "$APPDIR/usr/bin/ffmpeg" --executable "$APPDIR/usr/bin/ffprobe" \
    --custom-apprun "$ROOT/packaging/linux/AppRun" \
    --desktop-file "$APPDIR/usr/share/applications/schneidi.desktop" \
    --icon-file "$APPDIR/usr/share/icons/hicolor/scalable/apps/schneidi.svg" \
    --plugin qt
# Qt plugins only there because the build machine has them installed (kimageformats, virtual keyboard: ~20 MB with
# their libraries) out, then packed
python3 -I "$ROOT/packaging/linux/prune-appdir.py" "$APPDIR"
"$TOOLS/linuxdeploy-$ARCH.AppImage" --appdir "$APPDIR" --output appimage
APPIMAGE="$(ls -1t "$BUILD"/out/schneidi*.AppImage | head -1)"
if [ "$GPL_ENCODERS" = 0 ]; then
    # The package itself must load nothing GPL (FFmpeg's license, libraries in the process)
    if ! env -u LD_LIBRARY_PATH QT_QPA_PLATFORM=offscreen "$APPIMAGE" cli --licenses >"$BUILD/licenses.json"; then
        echo "!! GPL parts in the package:" >&2
        cat "$BUILD/licenses.json" >&2
        exit 1
    fi
    if find "$APPDIR" -name 'libx26[45]*' -o -name 'libpostproc*' -o -name 'librubberband*' -o -name 'libvidstab*' | grep -q .; then
        echo "!! GPL library in the AppDir" >&2
        exit 1
    fi
else
    # x264/x265 really in the package and found by schneidi (no download offer)
    for lib in libx264 libx265; do
        find "$APPDIR" -name "$lib.so*" | grep -q . || { echo "!! $lib missing in the AppDir" >&2; exit 1; }
    done
    env -u LD_LIBRARY_PATH QT_QPA_PLATFORM=offscreen "$APPIMAGE" cli --licenses >"$BUILD/licenses.json" || true
    grep -q '"x264' "$BUILD/licenses.json" || { echo "!! schneidi-cli --licenses does not list x264" >&2; exit 1; }
fi
NAME="$(basename "$APPIMAGE" .AppImage)$SUFFIX.AppImage"
mv -f "$APPIMAGE" "$DIST/$NAME"
APPIMAGE="$DIST/$NAME"
echo ">> fertig: $APPIMAGE ($(du -h "$APPIMAGE" | cut -f1))"
