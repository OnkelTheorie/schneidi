#!/usr/bin/env bash
# Builds dist/schneidi-<version>-x86_64.AppImage (release build + Qt, MLT modules, frei0r, ffmpeg/ffprobe).
# SCHNEIDI_NO_GPL=1: without anything GPL (own LGPL FFmpeg, no frei0r plugins) -> …-x86_64-nogpl.AppImage
# AppDir layout like the Windows program folder (see src/engine/Bundle.h):
#   usr/bin/{schneidi,schneidi-cli,ffmpeg,ffprobe}  usr/lib/mlt-7  usr/share/mlt-7  usr/lib/frei0r-1
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

# System-Ordner von MLT und frei0r (über pkg-config, Debian-Standard als Rückfall)
MLT_LIBDIR="$(pkg-config --variable=libdir mlt-framework-7 2>/dev/null || echo "/usr/lib/$MULTIARCH")"
MLT_MODULES="$MLT_LIBDIR/mlt-7"
MLT_SHARE="$(pkg-config --variable=datadir mlt-framework-7 2>/dev/null || echo /usr/share)/mlt-7"
[ -d "$MLT_SHARE" ] || MLT_SHARE=/usr/share/mlt-7
FREI0R_DIR=""
for d in "/usr/lib/frei0r-1" "/usr/lib/$MULTIARCH/frei0r-1" "/usr/local/lib/frei0r-1"; do
    [ -d "$d" ] && FREI0R_DIR="$d" && break
done

# Only the modules schneidi uses, all LGPL. No GPL module: schneidi has its own qtblend/qtcrop/qimage/qtext
# (engine/video Compose, Still, Text) and volume (engine/audio/Volume); normalize, resample and plus are GPL and
# not loaded (engine/Bundle.cpp)
MODULES=(core avformat sdl2 rtaudio frei0r xml)
# frei0r: all plugins (Effects Library, Open FX → frei0r; off by default) except face detection (would pull in OpenCV)
FREI0R_SKIP=(facebl0r facedetect)

# SCHNEIDI_NO_GPL=1: nothing GPL in the package (0.14). FFmpeg from packaging/linux/build-ffmpeg-lgpl.sh (OpenH264,
# kvazaar, SVT-AV1 … instead of x264/x265; FFMPEG_PREFIX, built here if missing), no frei0r plugins (GPL; the setting
# still finds plugins the user installs), checked at the end with `schneidi-cli --licenses`.
NO_GPL="${SCHNEIDI_NO_GPL:-0}"
CMAKE_EXTRA=()
FFMPEG_BIN_DIR=""
if [ "$NO_GPL" = 1 ]; then
    FFMPEG_PREFIX="${FFMPEG_PREFIX:-$ROOT/build-ffmpeg-lgpl/prefix}"
    [ -x "$FFMPEG_PREFIX/bin/ffmpeg" ] || "$ROOT/packaging/linux/build-ffmpeg-lgpl.sh" "$FFMPEG_PREFIX"
    export PKG_CONFIG_PATH="$FFMPEG_PREFIX/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
    # linuxdeploy and the system MLT's avformat module resolve libavcodec … to these (same sonames as Debian's)
    export LD_LIBRARY_PATH="$FFMPEG_PREFIX/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    CMAKE_EXTRA+=(-DSCHNEIDI_NO_GPL=ON)
    FFMPEG_BIN_DIR="$FFMPEG_PREFIX/bin"
    FREI0R_DIR=""
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

# --- MLT, frei0r, ffmpeg dazulegen ---
mkdir -p "$APPDIR/usr/lib/mlt-7" "$APPDIR/usr/share" "$APPDIR/usr/bin"
for m in "${MODULES[@]}"; do
    so="$MLT_MODULES/libmlt$m.so"
    if [ -f "$so" ]; then cp -L "$so" "$APPDIR/usr/lib/mlt-7/"; else echo "!! MLT-Modul fehlt: $so" >&2; fi
done
cp -rL "$MLT_SHARE" "$APPDIR/usr/share/mlt-7"
# Descriptions of modules that are not in the package (GPL ones among them)
for d in "$APPDIR/usr/share/mlt-7"/*/; do
    m="$(basename "$d")"
    case "$m" in profiles|presets|lumas|core|avformat|sdl2|rtaudio|frei0r|xml) ;; *) [ -f "$MLT_MODULES/libmlt$m.so" ] && rm -rf "$d" ;; esac
done
rm -rf "$APPDIR/usr/share/mlt-7"/{qt,qt6,plusgpl,glaxnimate,vid.stab,rubberband}
if [ "$NO_GPL" = 1 ]; then
    : # no frei0r plugins
elif [ -n "$FREI0R_DIR" ]; then
    mkdir -p "$APPDIR/usr/lib/frei0r-1"
    for so in "$FREI0R_DIR"/*.so; do
        [[ " ${FREI0R_SKIP[*]} " == *" $(basename "$so" .so) "* ]] || cp -L "$so" "$APPDIR/usr/lib/frei0r-1/"
    done
else
    echo "!! frei0r nicht gefunden (Green Screen und frei0r-Effekte fehlen im Paket)" >&2
fi
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
DEPS_ARGS=()
for f in "$APPDIR"/usr/lib/mlt-7/*.so "$APPDIR"/usr/lib/frei0r-1/*.so; do
    [ -e "$f" ] && DEPS_ARGS+=(--deploy-deps-only "$f")
done

mkdir -p "$DIST"
cd "$DIST"
"$TOOLS/linuxdeploy-$ARCH.AppImage" --appdir "$APPDIR" \
    --executable "$APPDIR/usr/bin/schneidi-cli" \
    --executable "$APPDIR/usr/bin/ffmpeg" --executable "$APPDIR/usr/bin/ffprobe" \
    --custom-apprun "$ROOT/packaging/linux/AppRun" \
    "${DEPS_ARGS[@]}" \
    --desktop-file "$APPDIR/usr/share/applications/schneidi.desktop" \
    --icon-file "$APPDIR/usr/share/icons/hicolor/scalable/apps/schneidi.svg" \
    --plugin qt --output appimage
APPIMAGE="$(ls -1t "$DIST"/schneidi*.AppImage | head -1)"
if [ "$NO_GPL" = 1 ]; then
    # The package itself must load nothing GPL (MLT modules, FFmpeg's license, libraries in the process)
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
