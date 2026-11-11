#!/usr/bin/env bash
# Builds dist/schneidi-windows-<version>.zip: program folder with schneidi.exe (+ schneidi-cli.exe), Qt and ffmpeg/ffprobe,
# and dist/schneidi-setup-<version>.exe (Inno Setup), with the own slim FFmpeg of packaging/windows/build-ffmpeg-lgpl.sh.
# GPL_ENCODERS (like the AppImage):
#   1 (default, the GitHub download since 0.21): FFmpeg with x264/x265 (GPL-2.0+, MSYS2's x264/x265 DLLs in the
#     folder, GPL-2.0 text and their notices in licenses/). schneidi's own code is the same as in the GPL-free build.
#   0: nothing GPL (SCHNEIDI_NO_GPL; x264/x265 are an optional download: engine/Extensions "x264", offered by the
#     installer) -> …-lgpl.zip / …-lgpl.exe in build-win-lgpl/. SCHNEIDI_NO_GPL=1 in the environment means the same.
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

# FFmpeg: the slim one of packaging/windows/build-ffmpeg-lgpl.sh (components: packaging/ffmpeg-components.txt) in
# FFMPEG_PREFIX, built there when it has no ffmpeg.exe. Checked at the end: GPL_ENCODERS=0 with `schneidi-cli
# --licenses` and by the DLL names in the program folder, GPL_ENCODERS=1 for x264/x265 in the folder.
GPL_ENCODERS="${GPL_ENCODERS:-1}"
[ "${SCHNEIDI_NO_GPL:-}" = 1 ] && GPL_ENCODERS=0
case "$GPL_ENCODERS" in 0 | 1) ;; *) echo "!! GPL_ENCODERS must be 0 or 1" >&2; exit 1 ;; esac
export GPL_ENCODERS # for build-ffmpeg-lgpl.sh
CMAKE_EXTRA=()
SUFFIX=""
if [ "$GPL_ENCODERS" = 1 ]; then
    FFMPEG_PREFIX="$(realpath -m "${FFMPEG_PREFIX:-$ROOT/build-ffmpeg/prefix}")"
    CMAKE_EXTRA+=(-DSCHNEIDI_NO_GPL=OFF)
else
    FFMPEG_PREFIX="$(realpath -m "${FFMPEG_PREFIX:-$ROOT/build-ffmpeg-lgpl/prefix}")"
    BUILD="$ROOT/build-win-lgpl"
    SUFFIX="-lgpl"
    CMAKE_EXTRA+=(-DSCHNEIDI_NO_GPL=ON)
fi
# (Built again also when the prefix lacks the licence texts or still has postproc: one of a build before 0.21)
if [ ! -x "$FFMPEG_PREFIX/bin/ffmpeg.exe" ] || [ ! -f "$FFMPEG_PREFIX/share/licenses/ffmpeg/LICENSE.md" ] ||
    find "$FFMPEG_PREFIX/bin" -iname 'postproc*.dll' 2>/dev/null | grep -q .; then
    "$ROOT/packaging/windows/build-ffmpeg-lgpl.sh" "$FFMPEG_PREFIX"
fi
if [ "$GPL_ENCODERS" = 1 ]; then
    encoders="$("$FFMPEG_PREFIX/bin/ffmpeg.exe" -hide_banner -encoders 2>/dev/null || true)"
    grep -q ' libx264 ' <<<"$encoders" || { echo "!! $FFMPEG_PREFIX has no libx264 (built with GPL_ENCODERS=0?)" >&2; exit 1; }
fi
export PKG_CONFIG_PATH="$FFMPEG_PREFIX/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
export PATH="$FFMPEG_PREFIX/bin:$PATH" # its DLLs: found first by the build checks and by ntldd below
FFMPEG_BIN="$FFMPEG_PREFIX/bin"
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
# Licence texts (MIT/BSD ask for their notice in every copy: earshot's recipe ports, RNNoise; the CC0 sounds' files)
mkdir -p "$OUT/licenses"
cp "$ROOT/LICENSE" "$ROOT/THIRD_PARTY.md" "$OUT/licenses/"
cp "$ROOT/third_party/earshot/LICENSE" "$OUT/licenses/earshot-LICENSE.txt"
cp "$ROOT/third_party/rnnoise/COPYING" "$OUT/licenses/rnnoise-COPYING.txt"
cp "$ROOT"/assets/sfx/cc0/LICENSE-* "$ROOT/assets/sfx/cc0/SOURCES.md" "$OUT/licenses/"
for f in "$FFMPEG_PREFIX"/share/licenses/ffmpeg/*; do cp "$f" "$OUT/licenses/ffmpeg-$(basename "$f")"; done
# Where the source of the GPL/LGPL parts is + written offer (release asset made by packaging/collect-gpl-sources.sh)
sed "s/@VERSION@/$VERSION/g" "$ROOT/packaging/SOURCE-OFFER.txt.in" >"$OUT/licenses/SOURCE-OFFER.txt"
if [ "$GPL_ENCODERS" = 1 ]; then
    # x264/x265 (GPL-2.0+, MSYS2 packages): GPL-2.0 text above (ffmpeg-COPYING.GPLv2), their own licence files if the
    # packages have them, and a notice with copyright, version and where the source is
    for lib in x264 x265; do
        [ -d "$UCRT/share/licenses/$lib" ] || continue
        for f in "$UCRT/share/licenses/$lib"/*; do cp "$f" "$OUT/licenses/$lib-$(basename "$f")"; done
    done
    {
        echo "x264 and x265 in this folder (the DLLs listed at the end; used by FFmpeg's avcodec DLL) are free"
        echo "software under the GNU General Public License version 2 or later (licenses/ffmpeg-COPYING.GPLv2)."
        echo "Because of them FFmpeg as built here is GPL-2.0-or-later too. schneidi itself is AGPL-3.0-only."
        echo
        echo "x264: Copyright (C) 2003 and later the x264 project authors (Laurent Aimar, Loren Merritt, Fiona Glaser …)."
        echo "      Source: https://code.videolan.org/videolan/x264"
        echo "x265: Copyright (C) 2013 and later MulticoreWare, Inc and the x265 authors (Steve Borho, Min Chen …)."
        echo "      Source: https://bitbucket.org/multicoreware/x265_git"
        echo
        echo "Source: licenses/SOURCE-OFFER.txt. Built from these MSYS2 packages (their source packages:"
        echo "https://repo.msys2.org/mingw/sources/, build scripts: https://github.com/msys2/MINGW-packages):"
        pacman -Q mingw-w64-ucrt-x86_64-libx264 mingw-w64-ucrt-x86_64-x265
    } >"$OUT/licenses/x264-x265-NOTICE.txt"
fi


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
# installed extension program (engine/Extensions.cpp provideRuntime); schneidi itself does not use them. ONNX Runtime
# 1.20 also imports msvcp140_1.dll (in vc_redist's vcRuntimeMinimum)
MSVC_RUNTIME_DIR="${MSVC_RUNTIME_DIR:-/c/schneidi/msvc-runtime}"
mkdir -p "$OUT/msvc-runtime"
for dll in vcruntime140.dll vcruntime140_1.dll msvcp140.dll msvcp140_1.dll vcomp140.dll; do
    # (no folder: the warning below, not an abort of the whole build through set -e/pipefail)
    src=""
    [ -d "$MSVC_RUNTIME_DIR" ] && src="$(find "$MSVC_RUNTIME_DIR" -maxdepth 1 -iname "$dll" | head -n1)"
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

# --- GPL_ENCODERS=0: nothing GPL in the package: FFmpeg's license and the libraries in the process (exit code 1
# otherwise), plus the DLLs in the folder by name (the bundled ffmpeg.exe is a process of its own) ---
if [ "$GPL_ENCODERS" = 0 ]; then
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
else
    # x264/x265 really in the folder and found by schneidi (no download offer)
    # Every x264/x265 DLL ntldd collected (x265 may come as libx265-*.dll plus libx265_main10/_main12 DLLs) is named
    # in the notice, and both are there
    X26X_DLLS="$(find "$OUT" -maxdepth 1 -iname 'libx26[45]*.dll' -printf '%f\n' | sort)"
    for lib in x264 x265; do
        grep -qi "^lib$lib" <<<"$X26X_DLLS" || { echo "!! lib$lib DLL missing in $OUT" >&2; exit 1; }
    done
    { echo; echo "The x264/x265 DLLs in this folder:"; echo "$X26X_DLLS"; } >>"$OUT/licenses/x264-x265-NOTICE.txt"
    QT_QPA_PLATFORM=offscreen "$OUT/schneidi-cli.exe" --licenses >"$BUILD/licenses.json" || true
    grep -q '"x264' "$BUILD/licenses.json" || { echo "!! schneidi-cli --licenses does not list x264" >&2; exit 1; }
    echo ">> license check: x264/x265 bundled ($BUILD/licenses.json)"
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
[ "$GPL_ENCODERS" = 0 ] && ISCC_EXTRA+=("/DGplFree=1") # x264/x265 not built in: offer the extension
MSYS2_ARG_CONV_EXCL="*" "$ISCC" /Q "/DVersion=$VERSION" "${ISCC_EXTRA[@]}" "/DSourceDir=$(cygpath -w "$OUT")" "/DOutputDir=$(cygpath -w "$DIST")" \
    "$(cygpath -w "$ROOT/packaging/windows/schneidi.iss")"
SETUP="$DIST/schneidi-setup-$VERSION$SUFFIX.exe"
echo ">> fertig: $SETUP ($(du -h "$SETUP" | cut -f1))"
