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

## Real phone (Galaxy A51, 22.11., arm64 APK of 273962f0)
- Tested by adb taps on the user's A51 (Android 13, 1080x2400 @420 = 411x914, 3-button nav bar), German UI. Shots
  `android-shots/50..66` (540 wide). Start: window visible after 1.45-1.7 s (`am start -W` TotalTime 0.5-0.65 s).
  GPU picture engine is off on the real device too ("no OpenGL device") -> CPU picture; playback ran in real time.
- Works: play/pause, playhead by tapping the ruler, split, Volume (Audio tab, slider drag), speed dialog (200 %), title
  via Text with the on-screen keyboard (live in the viewer), color sheet, export s264 720p30 8 s in 10 s (~24 fps
  incl. start): `~/projects/data/schneidi/phone-layout/a51-export.mp4` (h264 1280x720 30 fps 240 frames + AAC, 8.0 s,
  decodes clean), rotation (landscape full screen, back to portrait), silent restore after a kill (all edits back).
- Fixed on the phone: clip bar showed 2.5 tools ("Lauts…" cut in the middle: one long label made every button that
  wide) -> widths grow by one factor from each label; export sheet had no render queue ("Add to Render Queue" rendered
  nothing) -> queue below the settings in the sheet; "Cancel" in English (APK had no Qt translations) -> qtbase_*.qm
  of the UI languages in the resources; speed dialog cut its last check box -> dialogs grow to their contents once
  shown (Android only); color wheels scrolled the page instead of moving -> `touchDrags`; "Lautstärke" stayed
  checked while the Inspector was opened by Text -> a clip tool is checked only when it opened the panel.
- Seen, not fixed: the export location field shows the app folder again after a restart (the job keeps /sdcard/Movies);
  a dialog that grows stays left-anchored (not centered); the export sheet needs dragging up to reach "Alle rendern".

## Round of 22.11. evening (user feedback from the A51, import, layout forms)
- Phone timeline: compact track headers (60 px: V1 + eye/mute; locked/armed as small marks; tap = the header's menu
  with lock, hide/mute, voice-over, rename, color, tracks; no timecode box in the corner). One finger on an empty lane
  pans (both directions, glides on; never moves the playhead), on a clip it moves/trims at once, a long press on a clip
  is its menu, a hold on an empty lane then a drag is the rubber band; thick timeline scrollbars (26 px). Tool bar:
  a scroll area only follows the axes it scrolls (slanted swipes no longer jump). Tests in test_touch_input.
- Import on Android: content:// from the system picker - the file's path where readable, else a ContentResolver
  descriptor per open read by FFmpeg's fd protocol (FFmpeg rebuilt with --enable-protocol=fd; prefixes without it
  kept as android-deps/ffmpeg/<abi>/prefix-nofd), provider's display name, persistable permission
  (engine/ContentUri, OpenMedia::input). Dialog button rows wider than the screen stack (Android). test_content_uri.
- Own media gallery (ui/MediaGallery, engine/DeviceMedia): "+ Videos hinzufügen" over an empty timeline and
  Medien › Importieren… on phone/tablet; MediaStore videos/photos newest first by day, picked in order (numbers),
  filter, "Hinzufügen (n)", "Andere Datei…" = system picker; permission asked with an explanation (Android 14
  limited access counts), refused = picker; picks go into the pool and onto the timeline in order.
- Layout forms (MainWindow::wantedForm/setLayoutForm): Desktop (width >= 1100), Phone (shorter side < 600 and
  narrower than 1100), Tablet between (unfolded foldable, half a desktop screen); 24 px hysteresis; chosen at the start
  by the screen and switched live on resizes, fold/unfold and the setting "Anordnung" (Automatisch/Handy/Tablet/
  Desktop, ui/layoutForm, at once). Widgets move, nothing is rebuilt: ShellHomes records each widget's desktop place
  (splitter index, box index/stretch, grid cell, scroll area), the toggles, the page and the splitter sizes at the last
  desktop width; all come back exactly. Tablet: top bar across, tool rail, viewer above timeline, a panel docked at the
  right (Inspector first). Switches happen under a frozen picture that fades in 200 ms (ui/SwitchCurtain; spinner past
  0.5 s when events run); "Bewegungen reduzieren" (ui/reduceMotion), Android's remove-animations or
  SCHNEIDI_REDUCE_MOTION=1 switch without fade. Tests and --screenshot runs pin the desktop layout (check.h,
  main.cpp) unless SCHNEIDI_LAYOUT/SCHNEIDI_PHONE_LAYOUT is set. test_layout_switch (74 checks).
- Foldable: AVD schneidi_fold (pixel_fold, same android-35 image; `adb shell cmd device_state state 0|2`, screenshots
  per display `screencap -d <id>`): open = tablet form (80), folded = phone form on the outer display (81), unfolded
  again (82), project and view kept. Needs MemoryMax=6G and few reinstalls (OOM-killed at 6G after several).
- Also: Inspector tabs never cut, export folder remembered (Android: shared Movies), grown dialogs centered, each
  sheet keeps its own height (export opens tall), half-cut last tool as a swipe hint, proportional tool widths.
- Shots: 67-68 (pan, header menu), 70-78 (empty timeline button, permission, gallery, picker import), 80-82 (fold).

## Round of 22.11. night (CapCut-like bar and timeline)
- Tool bar like phones' own (the user compared with CapCut; no "Mehr" button): frameless items as wide as their
  label (min 60 px), small symbol over a small label in the text color, active in the accent with a line; the clip
  bar's back arrow fixed outside the scrolling part; QScroller tuned (light drag, fling, soft bounce at both ends,
  axis lock), a finger on a gliding bar only stops it, Android touch slop 8 dp.
- Add tile in the timeline's own look: empty timeline = dashed clip-shaped drop zone on V1 ("+ Videos hinzufügen");
  with clips a small light "+" square at the lanes' right edge at clip height (as CapCut). Phone/tablet only.
- Playhead fixed in the middle (TimelineView::setCenterPlayhead; Settings › Timeline › Abspielmarke in der Mitte:
  Automatisch = phone/tablet, Immer, Nie): sliding empty lanes or the ruler scrubs (glide on), playback keeps the line
  centred every frame, room before 0 and after the end, "00:03 / 00:16" at the ruler's left, short seconds on the
  ruler; clip drag/long press/tap unchanged; desktop default off and gets its own scroll back after the phone form.
  Documented as a wanted exception to the core rule in notes/architektur.md. Scrub sound: unchanged (the existing
  scrub-audio behaviour).
- Shots 90-92 (92 = CapCut reference beside schneidi). Tests: test_touch_input 185 (centre mode), test_phone_layout 690
  (bar labels whole, flick stop, fixed back arrow, add tile), test_layout_switch 78.
- Not yet: swipe tests on the real A51 (it was locked: only installed).

## Not done yet
- Landscape: the picture is small now (~70 px).
- Soft rebuild for design/language/UI size without a restart: not done. MainWindow builds its widgets and many
  connections in its constructor and main.cpp wires the one window (live server, back key, runners); a rebuild means
  either a second MainWindow with the project handed over (undo history lost, a second top-level window for a moment)
  or splitting the constructor into rebuildable content. UI size (QT_SCALE_FACTOR) cannot change in-process at all.
- Real CS2 clip import through the picker on the A51: built and installed, not yet driven (phone was locked).
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

## Fix pass 1 (22./23.11., the approved list of 11 items from phone-review-2026-11-22)
Status (updated with each commit):
- 2 Centre playhead too slippery: DONE (desktop-tested, not yet on the emulator). TouchGestures: speed = finger's way
  over the last 80 ms (0 if it stood still > 50 ms before the lift), glide only after a flick (low 0.7 / normal 0.35
  px/ms), time-based exponential friction (tau 140 / 325 ms, capped 3 / 6 px/ms), a finger down stops it and that
  touch is no tap. Centred mode: a slow scrub (< 0.45 px/ms, no glide) holds at clip starts/ends, 0/end, markers,
  in/out within 8 px, one Touch::tick() per edge. Setting Timeline › "Schwung der Timeline" Wenig (default) / Normal /
  Aus (timeline/momentum). Test testMomentumAndAxis in test_touch_input (216 checks green).
- 4 Axis lock: DONE (same commit): TouchGestures::setAxisLock (timeline on), axis chosen at the pan start, vertical
  only if |dy| > 1.7 |dx|.
- 1 Transport jitter: DONE. Viewer timecode in the system fixed font when "monospace" is no fixed-pitch family
  (Android), centred, minimum width = the timecode with every digit as the widest digit (grows only); the play button
  as wide as the wider of ▶/⏸. Test testSteadyTransport in test_phone_layout (play x and timecode width over frames).
- 3, 5, 6, 7, 8, 9, 10, 11: NOT STARTED.
- Not yet: emulator screenshots (to ~/projects/data/schneidi/phone-pass1-2026-11-22/), arm64 APK.

## Pass 1 (22.11. night, stopped for shutdown)
Done: item 2 (glide/axis lock/snap, setting Schwung) + item 4 (axis lock) in ec64f2cc.
WIP, unbuilt: item 1 timecode (Viewer.cpp/.h). Not started: 3, 5–11 (list in notes plan-0.26.md / phone-review REPORT.md).
