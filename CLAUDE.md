# schneidi

Eigener Videoschnitt-Editor, angelehnt an DaVinci Resolve (Aufbau, Bedienung, Optik), aber deutlich einfacher.
Im Zweifel: verhalten wie DaVinci.

- Anforderungen: `videoeditor-anforderungen.md`
- Plan/Architektur/Meilensteine: `docs/plan.md`
- Kernregel: Timeline-Zoom/Scroll ändern sich nur durch Nutzereingabe (siehe Plan).

## Stack
C++17, Qt 6 Widgets, MLT 7 (Vorschau/Mehrspur/Export über FFmpeg), CMake.

## Bauen
```
cmake -S . -B build -G Ninja && cmake --build build && ./build/schneidi
```
Abhängigkeiten (Debian): `build-essential cmake ninja-build pkg-config qt6-base-dev libmlt++-dev libmlt-dev`
