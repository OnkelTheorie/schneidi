# Android APK prototype: progress log (branch android-apk)

Details in German: `docs/dev/android.md` (section "Android-App als APK").

Outside the repo (not in git, rebuildable):
- Qt (aqtinstall, venv `android-qt/aqt-venv`): `~/projects/workbench/schneidi/android-qt/6.8.2/{gcc_64,android_x86_64,android_arm64_v8a}`
  and `6.10.3/{gcc_64,android_x86_64,android_arm64_v8a}`:
  `aqt install-qt linux desktop <v> linux_gcc_64 -O .`, `aqt install-qt all_os android <v> android_x86_64 -O .`, same for `android_arm64_v8a`
- Build folders, FFmpeg prefixes (`ffmpeg/<abi>/prefix`), logs, test clip: `~/projects/workbench/schneidi/android-deps/`
- A full JDK is needed (Debian has only the JRE): `export JAVA_HOME=~/development/jdk-21.0.12.1+1`
- Emulator: `~/Android/Sdk/emulator/emulator -avd schachi_test -no-window -no-audio -gpu swiftshader_indirect -no-snapshot-save`,
  adb always with `-s emulator-5554`.

## Build and run
```
export JAVA_HOME=~/development/jdk-21.0.12.1+1 ANDROID_WORK=~/projects/workbench/schneidi/android-deps JOBS=8
export QT_ANDROID_ROOT=~/projects/workbench/schneidi/android-qt/6.10.3        # or 6.8.2
packaging/android/build-android.sh x86_64 $ANDROID_WORK/build610-x86_64        # emulator
packaging/android/build-android.sh arm64-v8a $ANDROID_WORK/build610-arm64-v8a  # phones
ADB="$HOME/Android/Sdk/platform-tools/adb -s emulator-5554"; P=io.github.onkeltheorie.schneidi
$ADB install -r $ANDROID_WORK/build610-x86_64/schneidi-0.26.0-x86_64.apk
$ADB push $ANDROID_WORK/android-test.mp4 /sdcard/Movies/      # synthetic clip (ffmpeg testsrc2 + sine, 8 s)
$ADB shell pm grant $P android.permission.READ_MEDIA_VIDEO; $ADB shell pm grant $P android.permission.READ_MEDIA_AUDIO
$ADB shell am start -n $P/org.qtproject.qt.android.bindings.QtActivity -e applicationArguments /sdcard/Movies/android-test.mp4
$ADB exec-out screencap -p > android-shots/x.png
```
(The 6.8.2 builds are in `build-x86_64` / `build-arm64-v8a`, the default folders without the second argument.)

## Milestones
- [x] 1. Qt for Android installed; `packaging/android/hello` builds an APK that runs in the emulator (`android-shots/01-hello.png`).
- [x] 2. FFmpeg 7.1.5 LGPL for x86_64 and arm64-v8a (`packaging/android/build-ffmpeg-android.sh`): component list without
  external libraries, plus MediaCodec. miniaudio: bundled header (AAudio at run time).
- [x] 3. schneidi APK for x86_64 and arm64-v8a (38 / 37 MB). In the emulator: starts, imports the clip given as
  argument, shows it, plays it (`02`, `03-playing-a/b`). arm64 is built only, never run.
- [x] 4. Back key (`04-back-key.png`), on-screen keyboard (`05-keyboard.png`, `06-renamed.png`) checked; two touch
  bugs fixed (TouchGestures child widgets, keyboard after a tap that focuses a field). Safe areas: Qt 6.8 has none →
  target SDK 34 there; Qt 6.9+ targets 35.

## Open (next steps)
- Use **Qt 6.10.3**: with 6.8.2 buttons in Qt-drawn dialogs do not react to taps (the hello app shows it with a bare
  QDialog) and there are no safe areas. With 6.10.3 both work (`android-shots/07`, `08`). APK 82 MB (libraries stored
  uncompressed by the newer Gradle plugin) instead of 38 MB; same content.
- arm64-v8a APK: built with both Qt versions (6.10.3: `build610-arm64-v8a`, 77 MB; 6.8.2: 37 MB), never run on a
  device (the real phone was not touched).
- File dialog / `content://` URIs untested; the app does not ask for READ_MEDIA_* itself yet (tests: `pm grant`).
- Phone layout: the desktop layout on 914x411 logical px is cramped; some icons are boxes (symbol font missing on
  Android: ship a font). Portrait is locked out in the manifest.
- GPU: emulator (SwiftShader) says "GPU picture engine off: no OpenGL device" (second GL context fails) → CPU picture;
  check on a real device.
- No TLS (update check, AI chat): Qt for Android needs OpenSSL libraries in the APK.
- FFmpeg without x264/x265/OpenH264/SVT-AV1/vpx/dav1d/opus/lame; export (MediaCodec, s264) untested; QProcess paths
  (ffmpeg program for proxies, whisper) do not exist on Android.
- Release signing, AAB, launcher icon sizes.
- Desktop check done: `schneidi` and `test_touch_input` build and pass (146 checks) in `android-deps/build-desktop`.
