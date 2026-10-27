cmake_minimum_required(VERSION 3.21)
project(schneidi VERSION 0.12.0 LANGUAGES CXX)
# Version being worked on. While it differs from the released version above, the app shows "0.11.0 → 0.12.0".
# On release set both to the same number; after that, set this one to the next target.
set(SCHNEIDI_NEXT_VERSION 0.13.0)

# Build optimized when nothing is given (debug builds stutter in playback: own filters run per frame)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build-Typ" FORCE)
endif()

# Faster rebuilds when ccache is installed (sharing between git worktrees: base_dir and hash_dir in ccache's own
# config, see docs). A launcher given on the command line wins.
find_program(SCHNEIDI_CCACHE ccache)
if(SCHNEIDI_CCACHE AND NOT CMAKE_CXX_COMPILER_LAUNCHER)
    set(CMAKE_CXX_COMPILER_LAUNCHER ${SCHNEIDI_CCACHE})
    set(CMAKE_C_COMPILER_LAUNCHER ${SCHNEIDI_CCACHE})
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets Network)
# Linux: "show in file manager" over the freedesktop D-Bus interface (src/ui/FileManager.cpp); without it the folder opens
if(UNIX AND NOT APPLE)
    find_package(Qt6 QUIET COMPONENTS DBus)
endif()
find_package(PkgConfig REQUIRED)
# MLT liefert unter Linux und MSYS2 (Windows) eine .pc-Datei
pkg_check_modules(MLT REQUIRED IMPORTED_TARGET mlt++-7)
# Own audio engine (src/engine/audio): FFmpeg libraries directly, see docs/archiv/plan-0.10-ton.md
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavformat libavcodec libswresample libswscale libavutil)
# Time stretching with pitch correction (own audio engine)
pkg_check_modules(RUBBERBAND REQUIRED IMPORTED_TARGET rubberband)
# Sound devices of the own audio engine: miniaudio, a single header (Debian libminiaudio-dev, MSYS2 miniaudio)
find_path(MINIAUDIO_INCLUDE_DIR miniaudio.h PATH_SUFFIXES miniaudio REQUIRED)
find_package(Threads REQUIRED)
# MLT's module folder (Bundle::prepareMltEnvironment looks there which Qt modules exist); only that file needs it
pkg_get_variable(MLT_MODULE_DIR mlt-framework-7 moduledir)
set_property(SOURCE src/engine/Bundle.cpp APPEND PROPERTY COMPILE_DEFINITIONS SCHNEIDI_MLT_MODULE_DIR="${MLT_MODULE_DIR}")
if(WIN32)
    # Nicht paketierter Build: MLT-Module/-Daten aus MSYS2 (siehe engine/Bundle.cpp)
    pkg_get_variable(MLT_PREFIX mlt-framework-7 prefix)
    add_compile_definitions(SCHNEIDI_MLT_DEV_PREFIX="${MLT_PREFIX}")
endif()

# Git state for the log and the splash screen (buildinfo.h, refreshed on every build)
find_package(Git QUIET)
set(SCHNEIDI_BUILDINFO ${CMAKE_BINARY_DIR}/generated/buildinfo.h)
add_custom_target(schneidi_buildinfo
    COMMAND ${CMAKE_COMMAND} -DSRC=${PROJECT_SOURCE_DIR} -DOUT=${SCHNEIDI_BUILDINFO} -DGIT=${GIT_EXECUTABLE} -DVERSION=${PROJECT_VERSION}
            -DNEXT=${SCHNEIDI_NEXT_VERSION}
            -P ${PROJECT_SOURCE_DIR}/cmake/BuildInfo.cmake
    BYPRODUCTS ${SCHNEIDI_BUILDINFO}
    COMMENT "Build id")

# All program sources except main.cpp: shared by the app and the tests
add_library(schneidi_lib STATIC
    src/core/Types.h
    src/core/I18n.h               src/core/I18n.cpp
    src/core/Timecode.h
    src/core/TimelineOps.h        src/core/TimelineOps.cpp
    src/core/TrimFrames.h         src/core/TrimFrames.cpp
    src/core/Keyframes.h          src/core/Keyframes.cpp
    src/core/Accents.h            src/core/Accents.cpp
    src/core/MediaAspect.h        src/core/MediaAspect.cpp
    src/core/GradeWindows.h       src/core/GradeWindows.cpp
    src/core/Retime.h             src/core/Retime.cpp
    src/core/ProjectFormat.h      src/core/ProjectFormat.cpp
    src/core/Project.h            src/core/Project.cpp
    src/core/ProjectFile.h        src/core/ProjectFile.cpp
    src/core/LiveHost.h
    src/core/Selection.h
    src/core/PlaybackMeter.h
    src/core/Editor.h             src/core/Editor.cpp
    src/core/EditorSubtitles.cpp
    src/core/EditorCompound.cpp
    src/core/EditorAttributes.cpp
    src/core/EditorDetail.h
    src/core/EditorTracks.cpp
    src/core/EditorTrim.cpp
    src/core/EditorSource.cpp
    src/core/EditorTransitions.cpp
    src/core/EditorRetime.cpp
    src/core/EditorEffects.cpp
    src/core/EditorMulticam.cpp
    src/core/EffectRegistry.h     src/core/EffectRegistry.cpp
    src/core/EffectFolders.h      src/core/EffectFolders.cpp
    src/core/Presets.h            src/core/Presets.cpp
    src/core/Loudness.h           src/core/Loudness.cpp
    src/core/TruePeak.h           src/core/TruePeak.cpp
    src/core/Limiter.h            src/core/Limiter.cpp
    src/core/Dynamics.h           src/core/Dynamics.cpp
    src/core/Equalizer.h          src/core/Equalizer.cpp
    src/core/RenderJob.h          src/core/RenderJob.cpp
    src/core/Subtitles.h          src/core/Subtitles.cpp
    src/core/Transcript.h         src/core/Transcript.cpp
    src/core/TranscriptCut.h      src/core/TranscriptCut.cpp
    src/core/Fft.h                src/core/Fft.cpp
    src/core/AudioSync.h          src/core/AudioSync.cpp
    src/core/Beats.h              src/core/Beats.cpp
    src/core/Multicam.h           src/core/Multicam.cpp
    src/core/Interchange.h        src/core/Interchange.cpp
    src/core/InterchangeEdl.cpp   src/core/InterchangeFcp7.cpp  src/core/InterchangeOtio.cpp
    src/core/StreamPresets.h      src/core/StreamPresets.cpp
    src/core/StartTemplates.h     src/core/StartTemplates.cpp
    src/core/Ducking.h            src/core/Ducking.cpp
    src/core/ExportCheck.h        src/core/ExportCheck.cpp
    src/core/Stutter.h            src/core/Stutter.cpp
    src/core/ContactSheet.h       src/core/ContactSheet.cpp
    src/core/PoolFilter.h         src/core/PoolFilter.cpp
    src/core/WatchFolders.h       src/core/WatchFolders.cpp

    src/engine/Engine.h           src/engine/Engine.cpp
    src/engine/Background.h       src/engine/Background.cpp
    src/engine/PreviewConsumer.h  src/engine/PreviewConsumer.cpp
    src/engine/Profiles.h         src/engine/Profiles.cpp
    src/engine/TimelineBuilder.h  src/engine/TimelineBuilder.cpp
    src/engine/TimelineBuilderDetail.h src/engine/TimelineBuilderClip.cpp
    src/engine/TimelineBuilderAudio.cpp src/engine/TimelineBuilderTransitions.cpp
    src/engine/TimelineBuilderMulticam.cpp
    src/engine/LiveUpdate.h       src/engine/LiveUpdate.cpp
    src/engine/VideoEffects.h     src/engine/VideoEffects.cpp
    src/engine/RampProducer.h     src/engine/RampProducer.cpp
    src/engine/PitchLatency.h     src/engine/PitchLatency.cpp
    src/engine/Exporter.h         src/engine/Exporter.cpp
    src/engine/ExportWriter.h     src/engine/ExportWriter.cpp
    src/engine/SmartRender.h      src/engine/SmartRender.cpp
    src/engine/DecodeOrder.h      src/engine/DecodeOrder.cpp
    src/engine/OutputColor.h      src/engine/OutputColor.cpp
    src/engine/RateConverter.h    src/engine/RateConverter.cpp
    src/engine/HardwareEncoders.h src/engine/HardwareEncoders.cpp
    src/engine/Extensions.h       src/engine/Extensions.cpp
    src/engine/Transcriber.h      src/engine/Transcriber.cpp
    src/engine/MediaCache.h       src/engine/MediaCache.cpp
    src/engine/StillFetcher.h     src/engine/StillFetcher.cpp
    src/engine/ScrubFrames.h      src/engine/ScrubFrames.cpp
    src/engine/ProxyManager.h     src/engine/ProxyManager.cpp
    src/engine/ProxyCleanup.h     src/engine/ProxyCleanup.cpp
    src/engine/MemoryRelease.h    src/engine/MemoryRelease.cpp
    src/engine/RenderCache.h      src/engine/RenderCache.cpp
    src/engine/Denoise.h          src/engine/Denoise.cpp
    src/engine/Stabilizer.h       src/engine/Stabilizer.cpp
    src/engine/Preroll.h          src/engine/Preroll.cpp
    src/engine/AudioAnalysis.h    src/engine/AudioAnalysis.cpp
    src/engine/CheckMeasure.h     src/engine/CheckMeasure.cpp
    src/engine/FrameGaps.h        src/engine/FrameGaps.cpp
    src/engine/SoundSync.h        src/engine/SoundSync.cpp
    src/engine/ColorGrade.h       src/engine/ColorGrade.cpp
    src/engine/GradedThumbs.h     src/engine/GradedThumbs.cpp
    src/engine/BlurFill.h         src/engine/BlurFill.cpp
    src/engine/Tracker.h          src/engine/Tracker.cpp
    src/engine/Lumas.h            src/engine/Lumas.cpp
    src/engine/Frei0r.h           src/engine/Frei0r.cpp
    src/engine/RenderQueue.h      src/engine/RenderQueue.cpp
    src/engine/Snapshot.h         src/engine/Snapshot.cpp
    src/engine/audio/Reader.h     src/engine/audio/Reader.cpp
    src/engine/video/Reader.h     src/engine/video/Reader.cpp
    src/engine/video/Producer.h   src/engine/video/Producer.cpp
    src/engine/video/Hardware.h   src/engine/video/Hardware.cpp
    src/engine/audio/Mixer.h      src/engine/audio/Mixer.cpp
    src/engine/audio/Retimer.h    src/engine/audio/Retimer.cpp
    src/engine/audio/Path.h       src/engine/audio/Path.cpp
    src/engine/audio/Device.h     src/engine/audio/Device.cpp
    src/engine/audio/Player.h     src/engine/audio/Player.cpp
    src/engine/audio/Miniaudio.h  src/engine/audio/Miniaudio.cpp
    src/engine/audio/Wait.h       src/engine/audio/Wait.cpp
    src/engine/MixedSound.h       src/engine/MixedSound.cpp

    src/app/Theme.h               src/app/Theme.cpp
    src/app/InputBindings.h       src/app/InputBindings.cpp
    src/app/MainWindow.h          src/app/MainWindow.cpp
    src/app/MainWindowActions.cpp
    src/app/MainWindowLayout.cpp
    src/app/MainWindowProject.cpp src/app/MainWindowLive.cpp src/app/MainWindowSettings.cpp
    src/app/SettingsDialog.h src/app/SettingsDialog.cpp
    src/app/AiSettings.h src/app/AiSettings.cpp
    src/app/AiSettingsPage.h src/app/AiSettingsPage.cpp
    src/app/UpdateCheck.h src/app/UpdateCheck.cpp
    src/app/AiLog.h src/app/AiLog.cpp
    src/app/AiProvider.h src/app/AiProvider.cpp
    src/app/AiClient.h src/app/AiClient.cpp
    src/app/AiKeys.h src/app/AiKeys.cpp
    src/app/McpClient.h src/app/McpClient.cpp
    src/app/AiChat.h src/app/AiChat.cpp
    src/app/AiChatPanel.h src/app/AiChatPanel.cpp
    src/app/MainWindowChat.cpp
    src/app/MainWindowMulticam.cpp src/app/MainWindowExchange.cpp
    src/app/MainWindowAudio.cpp
    src/app/MainWindowWatch.cpp
    src/app/KeyBindingsDialog.h   src/app/KeyBindingsDialog.cpp
    src/app/ProjectSettingsDialog.h src/app/ProjectSettingsDialog.cpp
    src/app/ClipSpeedDialog.h src/app/ClipSpeedDialog.cpp
    src/app/ProxyCleanupDialog.h src/app/ProxyCleanupDialog.cpp
    src/app/NormalizeDialog.h src/app/NormalizeDialog.cpp
    src/app/StreamPresetsDialog.h src/app/StreamPresetsDialog.cpp
    src/app/StartTemplatesPage.h src/app/StartTemplatesPage.cpp
    src/app/DuckingDialog.h src/app/DuckingDialog.cpp
    src/app/MulticamDialog.h src/app/MulticamDialog.cpp
    src/app/ExtensionsDialog.h  src/app/ExtensionsDialog.cpp
    src/app/TranscribeDialog.h  src/app/TranscribeDialog.cpp
    src/app/PasteAttributesDialog.h src/app/PasteAttributesDialog.cpp
    src/app/DesignDialog.h src/app/DesignDialog.cpp
    src/app/EqDialog.h src/app/EqDialog.cpp
    src/app/DynamicsDialog.h src/app/DynamicsDialog.cpp
    src/app/TimelineExportDialog.h src/app/TimelineExportDialog.cpp
    src/app/ContactSheetDialog.h src/app/ContactSheetDialog.cpp
    src/app/FolderWatch.h src/app/FolderWatch.cpp
    src/app/WatchFoldersDialog.h src/app/WatchFoldersDialog.cpp
    src/app/Log.h                 src/app/Log.cpp
    src/app/CrashReport.h         src/app/CrashReport.cpp

    src/ui/MediaPool.h            src/ui/MediaPool.cpp
    src/ui/EffectsLibrary.h       src/ui/EffectsLibrary.cpp
    src/ui/MediaStorage.h         src/ui/MediaStorage.cpp
    src/ui/PoolFilterButton.h     src/ui/PoolFilterButton.cpp
    src/ui/HoverScrub.h           src/ui/HoverScrub.cpp
    src/ui/ElidedLabel.h
    src/ui/FileManager.h          src/ui/FileManager.cpp
    src/engine/Bundle.h           src/engine/Bundle.cpp
    src/ui/Viewer.h               src/ui/Viewer.cpp
    src/ui/ViewerOverlay.h
    src/ui/KeyColorPicker.h       src/ui/KeyColorPicker.cpp
    src/ui/WindowOverlay.h        src/ui/WindowOverlay.cpp
    src/ui/EqCurve.h              src/ui/EqCurve.cpp
    src/ui/MulticamGrid.h         src/ui/MulticamGrid.cpp
    src/ui/MulticamView.h         src/ui/MulticamView.cpp
    src/ui/TrimView.h             src/ui/TrimView.cpp
    src/ui/Inspector.h            src/ui/Inspector.cpp
    src/ui/InspectorDetail.h
    src/ui/InspectorSections.cpp
    src/ui/InspectorTitle.cpp
    src/ui/InspectorTransition.cpp
    src/ui/InspectorSubtitles.cpp
    src/ui/ColorPanel.h           src/ui/ColorPanel.cpp
    src/ui/Scopes.h               src/ui/Scopes.cpp
    src/ui/ScopesPanel.h          src/ui/ScopesPanel.cpp
    src/ui/Mixer.h                src/ui/Mixer.cpp
    src/ui/TranscriptPanel.h      src/ui/TranscriptPanel.cpp
    src/ui/ScrubField.h           src/ui/ScrubField.cpp
    src/ui/DeliverPanel.h         src/ui/DeliverPanel.cpp
    src/ui/RenderQueuePanel.h     src/ui/RenderQueuePanel.cpp
    src/ui/timeline/ViewState.h
    src/ui/timeline/TimelineView.h    src/ui/timeline/TimelineView.cpp
    src/ui/timeline/TimelineCurves.cpp
    src/ui/timeline/TimelinePaint.cpp
    src/ui/timeline/TimelineMouse.cpp
    src/ui/timeline/TimelineDrop.cpp
    src/ui/timeline/TimelineMenus.cpp
    src/ui/timeline/TimelineSubtitles.cpp
    src/ui/timeline/TimelineRetime.cpp
    src/ui/timeline/TimelineViewDetail.h
    src/ui/timeline/TimelinePanel.h   src/ui/timeline/TimelinePanel.cpp
)

# Per-pixel loops run on every exported frame: full vectorization (GCC/Clang -O2 only vectorizes loops whose length
# is a known multiple of the vector width). OutputColor: 3.5 -> 1.6 ms per 1080p frame.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set_source_files_properties(src/engine/OutputColor.cpp src/engine/video/Reader.cpp src/engine/BlurFill.cpp
                                PROPERTIES COMPILE_OPTIONS "$<$<NOT:$<CONFIG:Debug>>:-O3>")
endif()

# One dictionary per UI language: a new i18n/<code>.json is picked up by the build and the language menu by itself
file(GLOB SCHNEIDI_I18N RELATIVE ${PROJECT_SOURCE_DIR} CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/i18n/*.json)
qt_add_resources(schneidi_lib "i18n" PREFIX "/" FILES ${SCHNEIDI_I18N})
set(SCHNEIDI_THEMES assets/themes/dark-orange.json assets/themes/midnight.json assets/themes/graphit.json assets/themes/hell.json assets/themes/beige.json)
qt_add_resources(schneidi_lib "themes" PREFIX "/" BASE assets FILES ${SCHNEIDI_THEMES})
file(GLOB SCHNEIDI_LUTS RELATIVE ${PROJECT_SOURCE_DIR} CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/assets/luts/*.cube) # tools/gen_luts.py
qt_add_resources(schneidi_lib "luts" PREFIX "/" BASE assets FILES ${SCHNEIDI_LUTS})
target_include_directories(schneidi_lib PUBLIC src)
target_include_directories(schneidi_lib PRIVATE ${MINIAUDIO_INCLUDE_DIR})
target_link_libraries(schneidi_lib PUBLIC Qt6::Widgets Qt6::Network PkgConfig::MLT PkgConfig::FFMPEG PkgConfig::RUBBERBAND
                      Threads::Threads ${CMAKE_DL_LIBS})
if(TARGET Qt6::DBus)
    target_link_libraries(schneidi_lib PUBLIC Qt6::DBus)
    target_compile_definitions(schneidi_lib PRIVATE SCHNEIDI_HAVE_DBUS)
endif()
if(WIN32)
    target_link_libraries(schneidi_lib PUBLIC advapi32) # Credential Manager: the AI chat's key (app/AiKeys.cpp)
endif()
# Third-party code: its warnings are not ours
set_source_files_properties(src/engine/audio/Miniaudio.cpp PROPERTIES COMPILE_OPTIONS "-w")

add_executable(schneidi src/main.cpp)
target_include_directories(schneidi PRIVATE ${CMAKE_BINARY_DIR}/generated)
add_dependencies(schneidi schneidi_buildinfo)
target_link_libraries(schneidi PRIVATE schneidi_lib schneidi_cli_lib) # live connection (cli/Live.h)

# Command line + MCP server (no window): for scripts and AIs, see src/cli/main.cpp
add_library(schneidi_cli_lib STATIC
    src/cli/Commands.h src/cli/Commands.cpp src/cli/CommandsDetail.h src/cli/CommandsProject.cpp
    src/cli/CommandsWatch.cpp
    src/cli/EditOps.cpp src/cli/EditOpsTimelines.cpp src/cli/EditOpsLook.cpp
    src/cli/EditOpsAudio.cpp src/cli/EditOpsEdit.cpp src/cli/EditOpsMulticam.cpp
    src/cli/AnalysisDetail.h src/cli/Analysis.cpp src/cli/AnalysisAudio.cpp src/cli/AnalysisVideo.cpp
    src/cli/AnalysisHighlights.cpp src/cli/AnalysisCheck.cpp src/cli/AnalysisOverview.cpp src/cli/AnalysisBeats.cpp
    src/cli/Exchange.cpp
    src/cli/Mcp.h      src/cli/Mcp.cpp
    src/cli/Sandbox.h  src/cli/Sandbox.cpp
    src/cli/Http.h     src/cli/Http.cpp
    src/cli/Serve.h    src/cli/Serve.cpp src/cli/ServeDetail.h src/cli/ServeFiles.cpp
    src/cli/ServeJobs.h src/cli/ServeJobs.cpp
    src/cli/Live.h     src/cli/Live.cpp
    src/cli/Guide.h    src/cli/Guide.cpp
    src/cli/Setup.h    src/cli/Setup.cpp)
target_link_libraries(schneidi_cli_lib PUBLIC schneidi_lib)
# The Claude skill of the repo is the editing guide of every AI (cli/Guide.h): `guide` tool, setup, app chat
file(GLOB_RECURSE SCHNEIDI_SKILL RELATIVE ${PROJECT_SOURCE_DIR} CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/skills/schneidi/*.md)
qt_add_resources(schneidi_cli_lib "skill" PREFIX "/" BASE skills FILES ${SCHNEIDI_SKILL})
add_executable(schneidi-cli src/cli/main.cpp)
target_include_directories(schneidi-cli PRIVATE ${CMAKE_BINARY_DIR}/generated)
add_dependencies(schneidi-cli schneidi_buildinfo)
target_link_libraries(schneidi-cli PRIVATE schneidi_cli_lib)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # engine/video/Hardware.cpp: its setpriority() must also be the one the GPU driver's decoder library calls
    foreach(exe schneidi schneidi-cli)
        target_link_options(${exe} PRIVATE "LINKER:--export-dynamic-symbol=setpriority")
    endforeach()
endif()

if(WIN32)
    set_target_properties(schneidi PROPERTIES WIN32_EXECUTABLE ON)
    target_sources(schneidi PRIVATE packaging/windows/schneidi.rc) # Programmsymbol
endif()

# Installation (Paketierung: AppImage unter Linux, später Programmordner unter Windows, siehe README.md)
include(GNUInstallDirs)
install(TARGETS schneidi schneidi-cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(UNIX AND NOT APPLE)
    install(FILES packaging/linux/schneidi.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    install(FILES assets/schneidi.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
    # Debian policy: /usr/share/doc/<package>/copyright (DEP-5) and changelog.gz
    set(SCHNEIDI_MAINTAINER "schneidi project <324913804+OnkelTheorie@users.noreply.github.com>")
    set(SCHNEIDI_HOMEPAGE "https://github.com/OnkelTheorie/schneidi")
    configure_file(packaging/linux/copyright.in ${CMAKE_BINARY_DIR}/packaging/copyright @ONLY)
    # Honors SOURCE_DATE_EPOCH for reproducible packages
    string(TIMESTAMP SCHNEIDI_CHANGELOG_DATE "%a, %d %b %Y %H:%M:%S +0000" UTC)
    configure_file(packaging/linux/changelog.in ${CMAKE_BINARY_DIR}/packaging/changelog @ONLY)
    file(ARCHIVE_CREATE OUTPUT ${CMAKE_BINARY_DIR}/packaging/changelog.gz
        PATHS ${CMAKE_BINARY_DIR}/packaging/changelog FORMAT raw COMPRESSION GZip)
    install(FILES ${CMAKE_BINARY_DIR}/packaging/copyright ${CMAKE_BINARY_DIR}/packaging/changelog.gz
        DESTINATION ${CMAKE_INSTALL_DOCDIR})
endif()

# Debian-Paket (schlank, Systembibliotheken statt Bundling): cmake --build build && cpack --config build/CPackConfig.cmake -G DEB
# Ausgabe nach dist/ (gitignored). Abhängigkeiten ermittelt dpkg-shlibdeps aus der Binärdatei.
if(UNIX AND NOT APPLE)
    set(CPACK_GENERATOR "DEB")
    set(CPACK_PACKAGE_NAME "schneidi")
    set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Simple video editor inspired by DaVinci Resolve")
    set(CPACK_PACKAGE_CONTACT "${SCHNEIDI_MAINTAINER}")
    set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "${SCHNEIDI_HOMEPAGE}")
    set(CPACK_PACKAGE_FILE_NAME "schneidi_${PROJECT_VERSION}_amd64")
    set(CPACK_STRIP_FILES ON)
    set(CPACK_PACKAGE_DIRECTORY "${CMAKE_BINARY_DIR}")
    set(CPACK_OUTPUT_FILE_PREFIX "${PROJECT_SOURCE_DIR}/dist")
    set(CPACK_DEBIAN_PACKAGE_SECTION "video")
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
    set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "amd64")
    # MLT modules (FFmpeg, frei0r, ...) are loaded at runtime, not linked; frei0r effects are optional.
    # miniaudio (own audio engine) opens ALSA/PulseAudio with dlopen, invisible to shlibdeps.
    set(CPACK_DEBIAN_PACKAGE_DEPENDS "libmlt-data, libmlt7, libasound2t64 | libasound2")
    # Hardware decoding on AMD/Intel (VAAPI) loads the driver at runtime; NVIDIA comes with its driver (CUDA)
    set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "frei0r-plugins, ffmpeg, libpulse0, mesa-va-drivers | va-driver")
    include(CPack)
endif()

# Automatische Tests (CTest): cmake -DSCHNEIDI_TESTS=ON, siehe README.md
option(SCHNEIDI_TESTS "Tests bauen" OFF)
if(SCHNEIDI_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
