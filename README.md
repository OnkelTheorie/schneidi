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

**Windows:** über [MSYS2](https://www.msys2.org), Umgebung UCRT64. Repo in einen Ordner ohne Leerzeichen/Umlaute klonen, dann:

```bash
pacman -S --needed mingw-w64-ucrt-x86_64-{toolchain,cmake,ninja,pkgconf,qt6-base,qt6-svg,mlt,ffmpeg,frei0r-plugins,SDL2,ntldd,libebur128,fftw,libsamplerate,rubberband,rtaudio}
packaging/windows/build-windows.sh
```

Ergebnis: `dist/schneidi-windows-<version>.zip` (Programmordner) und, falls [Inno Setup 6](https://jrsoftware.org/isinfo.php) installiert ist (`winget install JRSoftware.InnoSetup`), `dist/schneidi-setup-<version>.exe`.

**Tests:**

```bash
cmake -S . -B build-tests -G Ninja -DSCHNEIDI_TESTS=ON && cmake --build build-tests && ctest --test-dir build-tests --output-on-failure
```

## Hinweis

schneidi ist ein unabhängiges Projekt und steht in keiner Verbindung zu Blackmagic Design. „DaVinci Resolve“ ist eine Marke von Blackmagic Design.

## Lizenz

[GPL-3.0](LICENSE)
