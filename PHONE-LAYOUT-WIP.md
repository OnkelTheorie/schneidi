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

## Done on 22.11. (second round)
- Android build of this branch (Qt 6.10.3, x86_64 `android-deps/build610-x86_64-phone`, arm64 `build610-arm64-phone`),
  built from a source snapshot `android-deps/src-phone` (`git ls-files` + the RNNoise tarball, rsync -c before each
  build) so that editing the worktree never mixes into a running build. Runs in the emulator, portrait and landscape.
- Fixes found on the emulator:
  - Boxes instead of symbols (undo/redo ↶ ↷, ✕, ☰): the fonts were loaded, but Android's platform theme hands buttons
    and labels a class font naming only "Roboto" (the app font's family list is lost). `Theme::apply` now also inserts
    the symbol fonts as substitutes of the system family (`QFont::insertSubstitutions`).
  - Landscape: the panel had only the viewer's height (~150 px, one row); now it is a column at the right over the
    whole height (bar, viewer and timeline in the left column).
  - Panel titles doubled (sheet header "Inspector" + panel title "Inspector"; "Color Corrector" squeezed the palette
    tabs): the panels' own `PanelTitle` labels are hidden in the sheet.
  - Tool labels elided ("Ins…tor"): portrait buttons never narrower than their label (the bar scrolls); a tapped tool
    scrolls fully into view.
  - Color palette tabs elided ("W…s"): touch mode's style-sheet `min-width` is an explicit minimum that beat the text
    width; set from the size hint once shown. The grade on/off dot no longer stretches into a red bar.
  - Viewer's "Timeline" title line hidden in compact mode unless it says more (source clip, proxy, recording).
  - Speed tool said "Easing" in English (key "Tempo" means easing there): now "Geschwindigkeit"/"Speed".
  - Viewer notices wrap on a narrow viewer (a long notice reached past the window).
  - The tool bar's last tools were off-screen without a hint: when the tools do not fit, the last one in sight is cut
    in half (`39-phone-portrait-swipe-hint`).
- Background jobs (proxies, renders ...): `BackgroundJobs::setCompact` (bar over "34 %" and ✕) sits in the phone's top
  bar between title and undo; a tap shows the full text as a viewer notice (no tooltips on touch).
- Volume clip tool: opens the Inspector on its Audio tab at the top (Volume section); a clip without sound keeps its tab
  and shows "Der ausgewählte Clip hat keinen Ton." (`Inspector::showAudio`, `PhoneShell::addClipPanelTool(..., opened)`).
- Settings › Oberfläche › Handy-Layout: Automatisch / Immer / Nie (`ui/phoneLayout` ""/on/off), after a restart
  (offered when it changes the layout). de+en strings only.
- Tests: `test_phone_layout` 542 checks (new: Volume → Audio tab, jobs in the top bar, setting, every clip tool
  reachable, palette tabs not cut, last tool cut in half), `test_touch_input` 146, `test_window_fit` 74, also `test_color` 87,
  `test_grade_qualifier`, `test_grade_windows`, `test_settings_layout`, `test_ui_scale` green.
- Emulator shots (Qt 6.10.3, x86_64, 1080x2400 @420 dpi = 411x914): `android-shots/30-phone-portrait-start`,
  `31-…-media`, `31b-…-media-scrolled`, `32-…-clip-bar`, `33-…-volume` (Audio tab), `34-…-color`, `34b-…-effects`,
  `35-…-export`, `36-phone-landscape-export`, `37-…-landscape-start`, `38-…-landscape-media`, `39-…-swipe-hint`.
- APKs of 82de4ef1: `android-deps/build610-x86_64-phone/schneidi-0.26.0-x86_64.apk` (83 MB),
  `android-deps/build610-arm64-phone/schneidi-0.26.0-arm64-v8a.apk` (77 MB; FFmpeg LGPL, no x264/x265).
- Emulator note: it was OOM-killed twice at 4G/5G (each reinstall grows it); 6G holds. Rotation needs
  `adb shell cmd window user-rotation lock 0|1` (the settings keys alone did not turn it).

## Done on 22.11. (third round)
- Landscape: the top bar is the head of the tool rail (⋮ ↶ ↷ in a row, the project name below, jobs below that;
  `MainWindow::phoneArrangeTopBar`, `PhoneShell::landscapeChanged`); no bar across the top. On Android the window goes
  full screen in landscape (status bar hidden, a swipe shows it; Android's own one-time "Viewing full screen" hint
  comes once) and back to maximized in portrait (only once the window is visible: the constructor's first layout
  looked landscape and made the window a small normal one). Viewer 1 : timeline 2, and the phone's rows start at
  56/46 px (`TimelineView::setTrackHeights`, touch mode had 84/68; a pinch on the headers still changes them): V2, V1
  and A1 whole on 914x411 (`42-phone-landscape-start`, `43-…-clip-rail`, `44-…-media`). The picture is small
  (~70 px high) in exchange.
- Restore after a kill: Android restores the backup without the question (the system ends apps in the background, the
  question came on almost every start) and says so in a viewer notice ("Nicht gespeicherte Änderungen von „%1“
  wiederhergestellt.", `46-phone-restored-notice`); desktop unchanged (asks). `MainWindow::restoresWithoutAsking()`,
  env `SCHNEIDI_SILENT_RESTORE=1/0` overrides. Android also writes the backup when the app goes to the background
  (`applicationStateChanged`), so a kill loses at most what changed since then. Undo or "close without saving" still
  drop a restored state as before.
- Media Pool on the phone: smaller tiles (96x54, `MediaPool::setCompactTiles`), two columns beside the bin list in
  portrait (`41-phone-portrait-media-two-columns`); desktop tiles unchanged.
- Shots: `40-phone-portrait-silent-restore` (no dialog), `41`, `42`-`44` landscape, `45-phone-portrait-after-turn`
  (status bar back), `46` (restore notice).
- Tests: `test_phone_layout` 584 checks (new: bar in the rail / one row in portrait, V1+A1 whole in landscape, two
  pool columns, silent restore without a dialog), `test_touch_input` 146, `test_window_fit` 74,
  `test_autosave_restore` 41, `test_proxy_menus` 86 green.

## Done on 22.11. (fourth round)
- Landscape: the rail's head (⋮ ↶ ↷, name) takes the rail's color (window), one surface with the camera cutout's
  strip at the screen edge (`48-phone-landscape-rail-color`).
- Media Pool on the phone: the generators (Text, Shape) after the clips (`MediaPool::setGeneratorsLast`, only the
  order shown; desktop keeps them first) (`47-phone-portrait-media-clips-first`).
- Docs (German, notes repo, not committed here): `dev/touch.md` (sections "Handy-Layout" and its pitfalls),
  `dev/android.md` (emulator memory/ANDROID_TMP/rotation, snapshot build, silent restore), code map row 33.
- Tests: `test_phone_layout` 590, `test_touch_input` 146, `test_window_fit` 74, `test_pool_storage` 200,
  `test_pool_remove` 91, `test_effects` 142 green.

## Not done yet
- Real phone (Galaxy A51): not on USB/adb this time (only Termux ssh over WLAN; wireless adb needs pairing on the
  phone). arm64 APK is built (`android-deps/build610-arm64-phone/schneidi-0.26.0-arm64-v8a.apk`), not run on a device.
- Landscape: the picture is small now (~70 px).
- Comp/Studio/Direct pages have no phone form; full-screen viewer is a separate top-level window (not checked).
- Color sheet has no viewer overlay windows; background job indicator not seen on Android (no proxy run there: the
  ffmpeg program does not exist on Android).
- Other languages for the 4 new strings (only de+en, translations at the end).

## Continue
```
cd ~/projects/workbench/schneidi/phone-layout
taskset -c 1-5,7-11 nice -n 19 cmake --build build -j8 --target schneidi test_phone_layout && ./build/tests/test_phone_layout
# offscreen screenshot in the phone layout:
SCHNEIDI_PHONE_LAYOUT=1 SCHNEIDI_SYMBOL_FONTS=1 QT_QPA_PLATFORM=offscreen SCHNEIDI_GPU=0 SCHNEIDI_AUDIO_DEVICE=null \
  SCHNEIDI_TEST_SETTINGS=~/projects/data/schneidi/phone-layout/settings timeout 60 ./build/schneidi \
  --screenshot x.png --size 411x914 --demo ~/projects/workbench/schneidi/android-deps/android-test.mp4 --actions page_color
# Android (ANDROID-WIP.md): snapshot, then build from it
S=~/projects/workbench/schneidi/android-deps/src-phone; git ls-files -z | rsync -a -c --from0 --files-from=- ./ $S/
cp third_party/rnnoise/rnnoise_data-*.tar.gz $S/third_party/rnnoise/   # once (ignored by git, else a download)
(cd $S && QT_ANDROID_ROOT=~/projects/workbench/schneidi/android-qt/6.10.3 JAVA_HOME=~/development/jdk-21.0.12.1+1 \
  ANDROID_WORK=~/projects/workbench/schneidi/android-deps JOBS=4 packaging/android/build-android.sh x86_64 \
  ~/projects/workbench/schneidi/android-deps/build610-x86_64-phone)
# emulator: systemd-run --user --scope -p MemoryMax=6G … emulator -avd schachi_test -no-window …
# rotation: adb shell cmd window user-rotation lock 0|1
```
