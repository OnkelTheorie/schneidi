#!/usr/bin/env bash
# Builds dist/schneidi-<version>-x86_64.AppImage (release build + Qt, ffmpeg/ffprobe).
# SCHNEIDI_NO_GPL=1: without anything GPL (own LGPL FFmpeg) -> …-x86_64-nogpl.AppImage
# AppDir layout like the Windows program folder (see src/engine/Bundle.h):
#   usr/bin/{schneidi,schneidi-cli,ffmpeg,ffprobe}
# Run from the project folder: packaging/linux/build-appimage.sh
# CLI from the AppImage: `schneidi-<version>-x86_64.AppImage cli help` or a link named schneidi-cli (packaging/linux/AppRun)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TOOLS="$ROOT/packaging/linux/tools"
BUILD="$ROOT/build-appimage"
APPDIR="$BUILD/AppDir"
DIST="$ROOT/dist"
ARCH="$(uname -m)"
MULTIARCH="$(gcc -print-multiarch 2>/dev/null || echo "$ARCH-linux-gnu")"

# SCHNEIDI_NO_GPL=1: nothing GPL in the package (0.14). FFmpeg from packaging/linux/build-ffmpeg-lgpl.sh (OpenH264,
# kvazaar, SVT-AV1 … instead of x264/x265; FFMPEG_PREFIX, built here if missing), checked at the end with
# `schneidi-cli --licenses`.
NO_GPL="${SCHNEIDI_NO_GPL:-0}"
CMAKE_EXTRA=()
FFMPEG_BIN_DIR=""
if [ "$NO_GPL" = 1 ]; then
    FFMPEG_PREFIX="${FFMPEG_PREFIX:-$ROOT/build-ffmpeg-lgpl/prefix}"
    [ -x "$FFMPEG_PREFIX/bin/ffmpeg" ] || "$ROOT/packaging/linux/build-ffmpeg-lgpl.sh" "$FFMPEG_PREFIX"
    export PKG_CONFIG_PATH="$FFMPEG_PREFIX/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
    # linuxdeploy resolves libavcodec … to these (same sonames as Debian's)
    export LD_LIBRARY_PATH="$FFMPEG_PREFIX/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    CMAKE_EXTRA+=(-DSCHNEIDI_NO_GPL=ON)
    FFMPEG_BIN_DIR="$FFMPEG_PREFIX/bin"
    BUILD="$ROOT/build-appimage-nogpl"
    APPDIR="$BUILD/AppDir"
fi

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
for t in ffmpeg ffprobe; do
    if [ -n "$FFMPEG_BIN_DIR" ]; then cp -L "$FFMPEG_BIN_DIR/$t" "$APPDIR/usr/bin/"; else cp -L "$(command -v "$t")" "$APPDIR/usr/bin/"; fi
done

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

mkdir -p "$DIST"
cd "$DIST"
"$TOOLS/linuxdeploy-$ARCH.AppImage" --appdir "$APPDIR" "${DEPLOY_EXTRA[@]}" \
    --executable "$APPDIR/usr/bin/schneidi-cli" \
    --executable "$APPDIR/usr/bin/ffmpeg" --executable "$APPDIR/usr/bin/ffprobe" \
    --custom-apprun "$ROOT/packaging/linux/AppRun" \
    --desktop-file "$APPDIR/usr/share/applications/schneidi.desktop" \
    --icon-file "$APPDIR/usr/share/icons/hicolor/scalable/apps/schneidi.svg" \
    --plugin qt --output appimage
APPIMAGE="$(ls -1t "$DIST"/schneidi*.AppImage | head -1)"
if [ "$NO_GPL" = 1 ]; then
    # The package itself must load nothing GPL (FFmpeg's license, libraries in the process)
    if ! QT_QPA_PLATFORM=offscreen "$APPIMAGE" cli --licenses >"$BUILD/licenses.json"; then
        echo "!! GPL parts in the package:" >&2
        cat "$BUILD/licenses.json" >&2
        exit 1
    fi
    if find "$APPDIR" -name 'libx26[45]*' -o -name 'libpostproc*' -o -name 'librubberband*' -o -name 'libvidstab*' | grep -q .; then
        echo "!! GPL library in the AppDir" >&2
        exit 1
    fi
    mv "$APPIMAGE" "${APPIMAGE%.AppImage}-nogpl.AppImage"
    APPIMAGE="${APPIMAGE%.AppImage}-nogpl.AppImage"
fi
echo ">> fertig: $APPIMAGE"
