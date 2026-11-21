# Android APK prototype: progress log (branch android-apk)

Outside the repo (not in git, rebuildable):
- Qt 6.8.2 (aqtinstall): `~/projects/workbench/schneidi/android-qt/6.8.2/{gcc_64,android_x86_64,android_arm64_v8a}`
  (`aqt-venv/bin/aqt install-qt linux desktop 6.8.2 linux_gcc_64 -O .`, `aqt install-qt all_os android 6.8.2 android_x86_64 -O .`, same for `android_arm64_v8a`)
- Build folders, FFmpeg prefixes, logs: `~/projects/workbench/schneidi/android-deps/`
- A full JDK is needed (Debian has only the JRE): `export JAVA_HOME=~/development/jdk-21.0.12.1+1`
- Emulator: `~/Android/Sdk/emulator/emulator -avd schachi_test -no-window -no-audio -gpu swiftshader_indirect -no-snapshot-save`
  (pid of the one started by the agent: `android-deps/emulator.pid`), adb always with `-s emulator-5554`.

## Milestones
- [x] 1. Qt for Android installed; `packaging/android/hello` builds an APK that runs in the emulator (`android-shots/01-hello.png`).
  Release APKs come out unsigned: `zipalign` + `apksigner` with `~/.android/debug.keystore` (see packaging/android/build-android.sh).
