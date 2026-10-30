#!/usr/bin/env bash
# Builds dist/schneidi-windows-<version>.zip: program folder with schneidi.exe (+ schneidi-cli.exe), Qt and ffmpeg/ffprobe.
# Runs in MSYS2 (environment UCRT64). Layout as in src/engine/Bundle.h:
#   schneidi.exe ffmpeg.exe ffprobe.exe *.dll  platforms/ styles/ …
# Aufruf aus dem Projektordner:  packaging/windows/build-windows.sh
# Pakete: siehe README.md (Abschnitt Windows)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD="$ROOT/build-win"
DIST="$ROOT/dist"
UCRT="${MINGW_PREFIX:-/ucrt64}"
VERSION="$(sed -n 's/^project(schneidi VERSION \([0-9.]*\).*/\1/p' "$ROOT/CMakeLists.txt")"
VERSION="${VERSION:-0.1.0}"
OUT="$BUILD/schneidi"

[ "${MSYSTEM:-}" = "UCRT64" ] || { echo "!! Bitte in der MSYS2-Umgebung UCRT64 starten" >&2; exit 1; }

# --- Release-Build ---
cmake -S "$ROOT" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD"

# --- Programmordner ---
rm -rf "$OUT"
mkdir -p "$OUT"
cp "$BUILD/schneidi.exe" "$BUILD/schneidi-cli.exe" "$OUT/"
strip "$OUT/schneidi.exe" "$OUT/schneidi-cli.exe"
cp "$UCRT/bin/ffmpeg.exe" "$UCRT/bin/ffprobe.exe" "$OUT/"


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

# --- Collect every other DLL from UCRT64 (also those of ffmpeg) ---
collect() {
    ntldd -R "$@" 2>/dev/null | tr '\\' '/' | sed -n 's|.*=> \(.*\.dll\) (0x.*|\1|p' | while read -r dll; do
        u="$(cygpath -u "$dll")"
        case "$u" in "$UCRT"/bin/*) [ -f "$OUT/$(basename "$u")" ] || cp "$u" "$OUT/" ;; esac
    done
}
TARGETS=("$OUT"/*.exe)
while IFS= read -r -d '' f; do TARGETS+=("$f"); done < <(find "$OUT" -mindepth 2 -name '*.dll' -not -path '*/lib/*' -print0)
collect "${TARGETS[@]}"
collect "$OUT"/*.dll # DLLs der eben kopierten DLLs (ntldd -R findet manche über Umwege nicht)

# --- ZIP ---
mkdir -p "$DIST"
ZIP="$DIST/schneidi-windows-$VERSION.zip"
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
MSYS2_ARG_CONV_EXCL="*" "$ISCC" /Q "/DVersion=$VERSION" "/DSourceDir=$(cygpath -w "$OUT")" "/DOutputDir=$(cygpath -w "$DIST")" \
    "$(cygpath -w "$ROOT/packaging/windows/schneidi.iss")"
SETUP="$DIST/schneidi-setup-$VERSION.exe"
echo ">> fertig: $SETUP ($(du -h "$SETUP" | cut -f1))"
