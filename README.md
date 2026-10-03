# schneidi

Ein schlanker Videoschnitt-Editor, angelehnt an DaVinci Resolve (Aufbau, Bedienung, Tastenkürzel), aber deutlich einfacher.

*A lightweight video editor modeled after DaVinci Resolve's Edit page — same layout and shortcuts, far fewer features. UI available in German and English.*

> Status: frühe Entwicklung (0.x). Projektdateien können sich zwischen Versionen noch ändern.

## Funktionen

- Mehrere Video- und Audiospuren, Schneiden, Trimmen, Verschieben, Snapping, Undo/Redo
- Timeline-Zoom und -Scroll ändern sich **nur durch Nutzereingabe** – nichts springt, wenn Clips kürzer werden
- Tastenbelegung wie in DaVinci, anpassbar
- Titel, Untertitel, Fades, Übergänge, Lautstärke, Loudness-Normalisierung
- Farbkorrektur, Kurven, LUTs, Effekte mit Keyframes
- Clip-Geschwindigkeit / Retime
- Proxy- und Render-Cache für flüssige Vorschau
- Export und Render-Warteschlange über FFmpeg
- Themes, Deutsch/Englisch

## Bauen

C++17, Qt 6 Widgets, MLT 7, CMake.

**Linux (Debian/Ubuntu):**

```bash
sudo apt install build-essential cmake ninja-build pkg-config qt6-base-dev libmlt++-dev libmlt-dev
cmake -S . -B build -G Ninja && cmake --build build && ./build/schneidi
```

**Windows:** über MSYS2 (UCRT64), siehe [`docs/windows.md`](docs/windows.md) und `packaging/windows/build-windows.sh`.

**Tests:**

```bash
cmake -S . -B build-tests -G Ninja -DSCHNEIDI_TESTS=ON && cmake --build build-tests && ctest --test-dir build-tests --output-on-failure
```

## Dokumentation

- [`docs/plan.md`](docs/plan.md) – Architektur und Meilensteine
- [`docs/davinci-shortcuts.md`](docs/davinci-shortcuts.md) – Tastenkürzel
- [`docs/dev-notes.md`](docs/dev-notes.md) – Entwickler-Notizen

## Hinweis

schneidi ist ein unabhängiges Projekt und steht in keiner Verbindung zu Blackmagic Design. „DaVinci Resolve“ ist eine Marke von Blackmagic Design.
