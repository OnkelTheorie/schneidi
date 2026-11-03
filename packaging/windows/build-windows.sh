#!/usr/bin/env bash
# Builds dist/schneidi-windows-<version>.zip: program folder with schneidi.exe (+ schneidi-cli.exe), Qt and ffmpeg/ffprobe,
# and dist/schneidi-setup-<version>.exe (Inno Setup), without anything GPL (the default since 0.16, like the AppImage;
# x264/x265 are an optional download: engine/Extensions "x264", offered by the installer).
# SCHNEIDI_NO_GPL=0: with MSYS2's full FFmpeg (GPL: x264/x265 built in) -> …-gpl.zip / …-gpl.exe in build-win-gpl/,
# for local comparisons only, not for releases.
# Runs in MSYS2 (environment UCRT64). Layout as in src/engine/Bundle.h:
#   schneidi.exe ffmpeg.exe ffprobe.exe *.dll  platforms/ styles/ …
# Run from the project folder:  packaging/windows/build-windows.sh
# Packages: see README.md (section Windows)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD="$ROOT/build-win"
DIST="$ROOT/dist"
UCRT="${MINGW_PREFIX:-/ucrt64}"
VERSION="$(sed -n 's/^project(schneidi VERSION \([0-9.]*\).*/\1/p' "$ROOT/CMakeLists.txt")"
VERSION="${VERSION:-0.1.0}"

[ "${MSYSTEM:-}" = "UCRT64" ] || { echo "!! Please run in the MSYS2 environment UCRT64" >&2; exit 1; }

# FFmpeg: the slim GPL-free one of packaging/windows/build-ffmpeg-lgpl.sh (OpenH264, kvazaar, SVT-AV1 … instead of
# x264/x265; components: packaging/ffmpeg-components.txt) in FFMPEG_PREFIX, built there when it has no ffmpeg.exe.
# Checked at the end with `schneidi-cli --licenses` and by the DLL names in the program folder.
NO_GPL="${SCHNEIDI_NO_GPL:-1}"
CMAKE_EXTRA=()
SUFFIX=""
FFMPEG_PREFIX="${FFMPEG_PREFIX:-}"
FFMPEG_BIN="$UCRT/bin"
if [ "$NO_GPL" = 1 ]; then
    FFMPEG_PREFIX="$(realpath -m "${FFMPEG_PREFIX:-$ROOT/build-ffmpeg-lgpl/prefix}")"
    [ -x "$FFMPEG_PREFIX/bin/ffmpeg.exe" ] || "$ROOT/packaging/windows/build-ffmpeg-lgpl.sh" "$FFMPEG_PREFIX"
    export PKG_CONFIG_PATH="$FFMPEG_PREFIX/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
    export PATH="$FFMPEG_PREFIX/bin:$PATH" # its DLLs: found first by the build checks and by ntldd below
    FFMPEG_BIN="$FFMPEG_PREFIX/bin"
    CMAKE_EXTRA+=(-DSCHNEIDI_NO_GPL=ON)
else
    FFMPEG_PREFIX=""
    BUILD="$ROOT/build-win-gpl"
    SUFFIX="-gpl"
    CMAKE_EXTRA+=(-DSCHNEIDI_NO_GPL=OFF)
fi
OUT="$BUILD/schneidi"

# pkg-config results stay in the CMake cache: another FFmpeg than last time -> configure again from scratch
STAMP="$BUILD/ffmpeg-prefix.txt"
if [ -f "$BUILD/CMakeCache.txt" ] && [ "$(cat "$STAMP" 2>/dev/null)" != "$FFMPEG_PREFIX" ]; then
    echo ">> other FFmpeg than the last build: configuring $BUILD again"
    rm -f "$BUILD/CMakeCache.txt"
fi
mkdir -p "$BUILD"
printf '%s' "$FFMPEG_PREFIX" >"$STAMP"

# --- Release build ---
cmake -S "$ROOT" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release "${CMAKE_EXTRA[@]}"
cmake --build "$BUILD"

# --- Programmordner ---
rm -rf "$OUT"
mkdir -p "$OUT"
cp "$BUILD/schneidi.exe" "$BUILD/schneidi-cli.exe" "$OUT/"
strip "$OUT/schneidi.exe" "$OUT/schneidi-cli.exe"
cp "$FFMPEG_BIN/ffmpeg.exe" "$FFMPEG_BIN/ffprobe.exe" "$OUT/"


# --- Qt (Plugins: platforms, styles, imageformats, …) ---
windeployqt6 --release --no-translations --no-system-d3d-compiler --no-opengl-sw --no-compiler-runtime \
    --dir "$OUT" "$OUT/schneidi.exe"

# Qt's own texts (Save/Discard/Cancel, context menus) in the UI languages: the app looks in translations/ next to
# the exe (core/I18n.cpp); windeployqt runs with --no-translations (it would take every language there is)
mkdir -p "$OUT/translations"
for lang in de es fr pl ru; do
    qm="$UCRT/share/qt6/translations/qtbase_$lang.qm"
    if [ -f "$qm" ]; then cp "$qm" "$OUT/translations/"; else echo "!! Qt-Übersetzung fehlt: $qm (pacman -S mingw-w64-ucrt-x86_64-qt6-translations)" >&2; fi
done

# offscreen: für den unsichtbaren Testlauf (--screenshot), wie im AppImage
cp "$UCRT/share/qt6/plugins/platforms/qoffscreen.dll" "$OUT/platforms/"

# MSVC runtime for the whisper/piper extensions (built with MSVC; a fresh Windows lacks it): Microsoft's redistributable
# DLLs, taken from vc_redist.x64.exe without installing it (docs/dev/windows.md). The app copies them next to an
# installed extension program (engine/Extensions.cpp provideRuntime); schneidi itself does not use them
MSVC_RUNTIME_DIR="${MSVC_RUNTIME_DIR:-/c/schneidi/msvc-runtime}"
mkdir -p "$OUT/msvc-runtime"
for dll in vcruntime140.dll vcruntime140_1.dll msvcp140.dll vcomp140.dll; do
    src="$(find "$MSVC_RUNTIME_DIR" -maxdepth 1 -iname "$dll" | head -n1)"
    if [ -n "$src" ]; then cp "$src" "$OUT/msvc-runtime/$dll"; else echo "!! MSVC runtime missing: $MSVC_RUNTIME_DIR/$dll (whisper/piper won't start on a fresh Windows)" >&2; fi
done

# --- Collect every other DLL from UCRT64 (also those of ffmpeg) ---
collect() {
    ntldd -R "$@" 2>/dev/null | tr '\\' '/' | sed -n 's|.*=> \(.*\.dll\) (0x.*|\1|p' | while read -r dll; do
        u="$(cygpath -u "$dll")"
        case "$u" in "$UCRT"/bin/* | "$FFMPEG_BIN"/*) [ -f "$OUT/$(basename "$u")" ] || cp "$u" "$OUT/" ;; esac
    done
}
TARGETS=("$OUT"/*.exe)
while IFS= read -r -d '' f; do TARGETS+=("$f"); done < <(find "$OUT" -mindepth 2 -name '*.dll' -not -path '*/lib/*' -print0)
collect "${TARGETS[@]}"
collect "$OUT"/*.dll # DLLs der eben kopierten DLLs (ntldd -R findet manche über Umwege nicht)

# --- Nothing GPL in the package: FFmpeg's license and the libraries in the process (exit code 1 otherwise), plus
# the DLLs in the folder by name (the bundled ffmpeg.exe is a process of its own) ---
if [ "$NO_GPL" = 1 ]; then
    if ! QT_QPA_PLATFORM=offscreen "$OUT/schneidi-cli.exe" --licenses >"$BUILD/licenses.json"; then
        echo "!! GPL parts in the package:" >&2
        cat "$BUILD/licenses.json" >&2
        exit 1
    fi
    GPL_DLLS="$(find "$OUT" -iname '*.dll' | grep -iE '/(lib)?(x26[45]|postproc|xvidcore|rubberband|vidstab|frei0r|fftw3f?)[-.0-9]*[^/]*\.dll$' || true)"
    if [ -n "$GPL_DLLS" ]; then
        echo "!! GPL library in the program folder:" >&2
        echo "$GPL_DLLS" >&2
        exit 1
    fi
    "$OUT/ffmpeg.exe" -hide_banner -L 2>&1 | grep -q "Lesser General Public License" \
        || { echo "!! the bundled ffmpeg.exe is not LGPL" >&2; exit 1; }
    echo ">> license check: nothing GPL ($BUILD/licenses.json)"
fi

# --- ZIP ---
mkdir -p "$DIST"
ZIP="$DIST/schneidi-windows-$VERSION$SUFFIX.zip"
rm -f "$ZIP"
(cd "$BUILD" && zip -qr9 "$ZIP" schneidi)
echo ">> fertig: $ZIP ($(du -h "$ZIP" | cut -f1))"

# --- Installer (Inno Setup 6, einmalig: winget install JRSoftware.InnoSetup) ---
ISCC="${ISCC:-}" # Pfad zur ISCC.exe lässt sich auch vorgeben
LOCAL="$([ -n "${LOCALAPPDATA:-}" ] && cygpath -u "$LOCALAPPDATA" || echo "/c/Users/$(id -un)/AppData/Local")"
for d in "$LOCAL/Programs/Inno Setup 6" "/c/Program Files/Inno Setup 6" "/c/Program Files (x86)/Inno Setup 6"; do
    [ -z "$ISCC" ] && [ -f "$d/ISCC.exe" ] && ISCC="$d/ISCC.exe"
done
if [ -z "$ISCC" ]; then
    echo "!! Inno Setup nicht gefunden – kein Installer (winget install JRSoftware.InnoSetup)" >&2
    exit 0
fi
# MSYS2 würde /DName=… sonst für einen Pfad halten und umschreiben
ISCC_EXTRA=("/Fschneidi-setup-$VERSION$SUFFIX")
[ "$NO_GPL" = 1 ] && ISCC_EXTRA+=("/DGplFree=1") # x264/x265 not built in: offer the extension
MSYS2_ARG_CONV_EXCL="*" "$ISCC" /Q "/DVersion=$VERSION" "${ISCC_EXTRA[@]}" "/DSourceDir=$(cygpath -w "$OUT")" "/DOutputDir=$(cygpath -w "$DIST")" \
    "$(cygpath -w "$ROOT/packaging/windows/schneidi.iss")"
SETUP="$DIST/schneidi-setup-$VERSION$SUFFIX.exe"
echo ">> fertig: $SETUP ($(du -h "$SETUP" | cut -f1))"
