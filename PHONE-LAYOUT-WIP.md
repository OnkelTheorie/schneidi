# Phone layout: progress (branch phone-layout, from android-apk)

## Built (desktop-verified, not yet on Android)
- `src/ui/phone/PhoneShell.*`: the phone arrangement. Portrait: top bar, viewer, timeline, tool bar at the bottom; a
  tool opens its panel as a sheet over the timeline (drag handle: taller/lower, pulled down = closed; one panel at a
  time). Landscape: tools as a rail at the left, viewer and open panel side by side, timeline below. Clip selected →
  clip bar (back arrow, Split, Volume, Speed, Effects, Color, Delete). Panels larger than the sheet scroll inside it.
  Minimum size: none before the first show, then at most the window's shorter side (rotation never sticks).
- `src/app/MainWindowPhone.cpp`: `MainWindow::phoneLayoutWanted()` (screen's shorter side < 600 logical px; setting
  `ui/phoneLayout` on/off; env `SCHNEIDI_PHONE_LAYOUT=1/0`), `enablePhoneLayout()` (desktop top/page/menu bar and the
  pages' splitters hidden; same widgets, Theme colors, page-bar look). Tools: Media, Effects, Text (adds a title +
  Inspector), Audio (mixer), Color, Inspector, Transcript, AI, Export. Top bar: ⋮ (all menus), project name, undo/redo.
  `showPage` in phone mode: Media/Color/Deliver = sheets, Edit closes, Comp/Studio/Direct = notice "not on phones yet".
  Back key (`backStep`): sheet, then clip bar (selection cleared), then the old steps. Desktop toggles/menus open the
  sheet. Tours off in phone mode.
- `TimelinePanel::setCompact` (slip/slide hidden, narrow timeline list, bar scrolls sideways), `Viewer::setCompact`
  (no "to start"/before-after buttons, not centered, picture stack never sets the height).
- Symbol glyphs (boxes on Android): `assets/fonts/schneidi-symbols-{a,b,c}.ttf` (DejaVu Sans / Noto Sans Symbols 2 /
  Noto Sans Math subsets, renamed; `tools/make_symbol_fonts.py`, licenses `assets/fonts/LICENSE.txt`), appended as
  fallback families in `Theme::apply` only on Android (or `SCHNEIDI_SYMBOL_FONTS=1`). Desktop fonts unchanged.
- Manifest: `screenOrientation="fullUser"` (follows rotation; was sensorLandscape).
- Test `tests/test_phone_layout.cpp`: 411x914, 914x411, 360x740, 914x380, with and without touch mode + style sheet;
  nothing outside the window, layout minimum fits, one panel per sheet, back order, toggles, pages, rotation.
  332 checks green. `test_touch_input` (146) and `test_window_fit` (74) green.
- Offscreen screenshots (desktop build, touch mode): `android-shots/20..23-offscreen-*.png`.

## Not done yet
- Android build + emulator/phone screenshots (portrait/landscape) with this branch: not run.
- Background jobs (proxy/render progress) live in the hidden page bar: nothing shows them on the phone.
- Comp/Studio/Direct pages have no phone form; full-screen viewer is a separate top-level window (check on Android).
- Volume tool opens the whole Inspector (not scrolled to the audio section); Color sheet has no viewer overlay windows.
- No settings UI for `ui/phoneLayout`; i18n: only de+en (2 new strings in en.json).
- Docs (`docs/dev/touch.md`, `android.md`, code map) not updated.

## Continue
```
cd ~/projects/workbench/schneidi/phone-layout
taskset -c 1-5,7-11 nice -n 19 cmake --build build -j8 --target schneidi test_phone_layout && ./build/tests/test_phone_layout
# offscreen screenshot in the phone layout:
SCHNEIDI_PHONE_LAYOUT=1 SCHNEIDI_SYMBOL_FONTS=1 QT_QPA_PLATFORM=offscreen SCHNEIDI_GPU=0 SCHNEIDI_AUDIO_DEVICE=null \
  SCHNEIDI_TEST_SETTINGS=~/projects/data/schneidi/phone-layout/settings timeout 60 ./build/schneidi \
  --screenshot x.png --size 411x914 --demo ~/projects/workbench/schneidi/android-deps/android-test.mp4 --actions page_color
# Android: see ANDROID-WIP.md (QT_ANDROID_ROOT=…/android-qt/6.10.3, build-android.sh x86_64 …/android-deps/build610-x86_64-phone);
# rotation: adb shell settings put system accelerometer_rotation 0; adb shell settings put system user_rotation 0|1
```
