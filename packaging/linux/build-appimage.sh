#!/usr/bin/env bash
# Baut dist/schneidi-<version>-x86_64.AppImage (Release-Build + Qt, MLT-Module, frei0r, ffmpeg/ffprobe).
# Aufbau im AppDir wie später der Windows-Programmordner (siehe src/engine/Bundle.h):
#   usr/bin/{schneidi,ffmpeg,ffprobe}  usr/lib/mlt-7  usr/share/mlt-7  usr/lib/frei0r-1
# Aufruf aus dem Projektordner: packaging/linux/build-appimage.sh
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

# Nur die Module, die schneidi nutzt (libmltqt.so = Qt5 bewusst nicht: zöge Qt5 mit hinein)
MODULES=(core avformat qt6 sdl2 rtaudio frei0r rubberband xml resample plus normalize)
FREI0R_PLUGINS=(bluescreen0r)

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
cmake -S "$ROOT" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
cmake --build "$BUILD"
DESTDIR="$APPDIR" cmake --install "$BUILD"

# --- MLT, frei0r, ffmpeg dazulegen ---
mkdir -p "$APPDIR/usr/lib/mlt-7" "$APPDIR/usr/share" "$APPDIR/usr/bin"
for m in "${MODULES[@]}"; do
    so="$MLT_MODULES/libmlt$m.so"
    if [ -f "$so" ]; then cp -L "$so" "$APPDIR/usr/lib/mlt-7/"; else echo "!! MLT-Modul fehlt: $so" >&2; fi
done
cp -rL "$MLT_SHARE" "$APPDIR/usr/share/mlt-7"
if [ -n "$FREI0R_DIR" ]; then
    mkdir -p "$APPDIR/usr/lib/frei0r-1"
    for f in "${FREI0R_PLUGINS[@]}"; do cp -L "$FREI0R_DIR/$f.so" "$APPDIR/usr/lib/frei0r-1/"; done
else
    echo "!! frei0r nicht gefunden (Green Screen fehlt im Paket)" >&2
fi
for t in ffmpeg ffprobe; do cp -L "$(command -v "$t")" "$APPDIR/usr/bin/"; done

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
    --executable "$APPDIR/usr/bin/ffmpeg" --executable "$APPDIR/usr/bin/ffprobe" \
    "${DEPS_ARGS[@]}" \
    --desktop-file "$APPDIR/usr/share/applications/schneidi.desktop" \
    --icon-file "$APPDIR/usr/share/icons/hicolor/scalable/apps/schneidi.svg" \
    --plugin qt --output appimage
echo ">> fertig: $(ls -1 "$DIST"/schneidi*.AppImage | tail -1)"
