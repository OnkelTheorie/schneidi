#!/usr/bin/env bash
# Baut dist/schneidi-windows-<version>.zip: Programmordner mit schneidi.exe, Qt, MLT-Modulen, frei0r und ffmpeg/ffprobe.
# Läuft in MSYS2 (Umgebung UCRT64). Aufbau wie in src/engine/Bundle.h (<prefix> = Programmordner):
#   schneidi.exe ffmpeg.exe ffprobe.exe *.dll  platforms/ styles/ …  lib/mlt-7  share/mlt-7  lib/frei0r-1
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

# Nur die MLT-Module, die schneidi nutzt (wie im AppImage)
MODULES=(core avformat qt6 sdl2 rtaudio frei0r rubberband xml resample plus normalize)
FREI0R_PLUGINS=(bluescreen0r)

[ "${MSYSTEM:-}" = "UCRT64" ] || { echo "!! Bitte in der MSYS2-Umgebung UCRT64 starten" >&2; exit 1; }

# --- Release-Build ---
cmake -S "$ROOT" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD"

# --- Programmordner ---
rm -rf "$OUT"
mkdir -p "$OUT/lib/mlt-7" "$OUT/lib/frei0r-1" "$OUT/share"
cp "$BUILD/schneidi.exe" "$OUT/"
strip "$OUT/schneidi.exe"
cp "$UCRT/bin/ffmpeg.exe" "$UCRT/bin/ffprobe.exe" "$OUT/"

for m in "${MODULES[@]}"; do
    dll="$UCRT/lib/mlt/libmlt$m.dll" # MSYS2: lib/mlt, im Paket lib/mlt-7 wie im AppImage
    if [ -f "$dll" ]; then cp "$dll" "$OUT/lib/mlt-7/"; else echo "!! MLT-Modul fehlt: $dll" >&2; fi
done
# Beschreibungen der Module (.yml) und Daten (Profile, Presets, …)
cp -r "$UCRT/share/mlt" "$OUT/share/mlt-7"
for f in "${FREI0R_PLUGINS[@]}"; do
    dll="$UCRT/lib/frei0r-1/$f.dll"
    if [ -f "$dll" ]; then cp "$dll" "$OUT/lib/frei0r-1/"; else echo "!! frei0r-Plugin fehlt: $dll (Green Screen)" >&2; fi
done

# --- Qt (Plugins: platforms, styles, imageformats, …) ---
windeployqt6 --release --no-translations --no-system-d3d-compiler --no-opengl-sw --no-compiler-runtime \
    --dir "$OUT" "$OUT/schneidi.exe"

# offscreen: für den unsichtbaren Testlauf (--screenshot), wie im AppImage
cp "$UCRT/share/qt6/plugins/platforms/qoffscreen.dll" "$OUT/platforms/"

# --- Alle übrigen DLLs aus UCRT64 einsammeln (auch die der MLT-Module/frei0r/ffmpeg) ---
collect() {
    ntldd -R "$@" 2>/dev/null | tr '\\' '/' | sed -n 's|.*=> \(.*\.dll\) (0x.*|\1|p' | while read -r dll; do
        u="$(cygpath -u "$dll")"
        case "$u" in "$UCRT"/bin/*) [ -f "$OUT/$(basename "$u")" ] || cp "$u" "$OUT/" ;; esac
    done
}
TARGETS=("$OUT"/*.exe "$OUT"/lib/mlt-7/*.dll "$OUT"/lib/frei0r-1/*.dll)
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
