# schneidi

Eigener Videoschnitt-Editor, angelehnt an DaVinci Resolve (Aufbau, Bedienung, Optik), aber deutlich einfacher.
Im Zweifel: verhalten wie DaVinci.

- Anforderungen: `videoeditor-anforderungen.md`
- Plan/Architektur/Meilensteine: `docs/plan.md`
- Entwickler-Notizen (Testen, Screenshots, gelöste Stolperfallen): `docs/dev-notes.md`
- Kernregel: Timeline-Zoom/Scroll ändern sich nur durch Nutzereingabe (siehe Plan).

- **Windows-Portabilität (verbindlich):** Ab jetzt so entwickeln, dass ein Windows-Umzug später ohne Umbau geht: keine hartcodierten Linux-Pfade (`/usr`, `/tmp`, `~`), Pfade/Config nur über `QStandardPaths`/`QDir`/`QFileInfo`, keine POSIX-only-Header oder -Aufrufe (`unistd.h`, `fork`, …) ohne `#ifdef`, externe Programme nur über `QProcess` ohne Shell-Annahmen, keine Abhängigkeit von Linux-only-Bibliotheken/-Filtern ohne Fallback. Details/Plan: `docs/plan.md` (M5).

## Stack
C++17, Qt 6 Widgets, MLT 7 (Vorschau/Mehrspur/Export über FFmpeg), CMake.

## Bauen
```
cmake -S . -B build -G Ninja && cmake --build build && ./build/schneidi
```
Abhängigkeiten (Debian): `build-essential cmake ninja-build pkg-config qt6-base-dev libmlt++-dev libmlt-dev`

Windows (MSYS2 UCRT64, Details `docs/windows.md`): `packaging/windows/build-windows.sh` → `dist/schneidi-windows-<version>.zip` + `dist/schneidi-setup-<version>.exe` (Inno Setup)
