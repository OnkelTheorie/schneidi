cmake_minimum_required(VERSION 3.21)
project(schneidi VERSION 0.5.0 LANGUAGES CXX)

# Ohne Angabe optimiert bauen (Debug-Builds ruckeln bei der Wiedergabe: eigene Filter laufen pro Frame)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build-Typ" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets)
find_package(PkgConfig REQUIRED)
# MLT liefert unter Linux und MSYS2 (Windows) eine .pc-Datei
pkg_check_modules(MLT REQUIRED IMPORTED_TARGET mlt++-7)
if(WIN32)
    # Nicht paketierter Build: MLT-Module/-Daten aus MSYS2 (siehe engine/Bundle.cpp)
    pkg_get_variable(MLT_PREFIX mlt-framework-7 prefix)
    add_compile_definitions(SCHNEIDI_MLT_DEV_PREFIX="${MLT_PREFIX}")
endif()

# Git-Stand fürs Log/„Über“ (buildinfo.h, bei jedem Build aktualisiert)
find_package(Git QUIET)
set(SCHNEIDI_BUILDINFO ${CMAKE_BINARY_DIR}/generated/buildinfo.h)
add_custom_target(schneidi_buildinfo
    COMMAND ${CMAKE_COMMAND} -DSRC=${PROJECT_SOURCE_DIR} -DOUT=${SCHNEIDI_BUILDINFO} -DGIT=${GIT_EXECUTABLE} -DVERSION=${PROJECT_VERSION}
            -P ${PROJECT_SOURCE_DIR}/cmake/BuildInfo.cmake
    BYPRODUCTS ${SCHNEIDI_BUILDINFO}
    COMMENT "Build-Kennung")

# All program sources except main.cpp: shared by the app and the tests
add_library(schneidi_lib STATIC
    src/core/Types.h
    src/core/I18n.h               src/core/I18n.cpp
    src/core/Timecode.h
    src/core/TimelineOps.h        src/core/TimelineOps.cpp
    src/core/TrimFrames.h         src/core/TrimFrames.cpp
    src/core/Keyframes.h          src/core/Keyframes.cpp
    src/core/Retime.h             src/core/Retime.cpp
    src/core/ProjectFormat.h      src/core/ProjectFormat.cpp
    src/core/Project.h            src/core/Project.cpp
    src/core/ProjectFile.h        src/core/ProjectFile.cpp
    src/core/Selection.h
    src/core/Editor.h             src/core/Editor.cpp
    src/core/EditorSubtitles.cpp
    src/core/EditorCompound.cpp
    src/core/EditorAttributes.cpp
    src/core/EffectRegistry.h     src/core/EffectRegistry.cpp
    src/core/EffectFolders.h      src/core/EffectFolders.cpp
    src/core/Presets.h            src/core/Presets.cpp
    src/core/Loudness.h           src/core/Loudness.cpp
    src/core/TruePeak.h           src/core/TruePeak.cpp
    src/core/RenderJob.h          src/core/RenderJob.cpp
    src/core/Subtitles.h          src/core/Subtitles.cpp

    src/engine/Engine.h           src/engine/Engine.cpp
    src/engine/Profiles.h         src/engine/Profiles.cpp
    src/engine/TimelineBuilder.h  src/engine/TimelineBuilder.cpp
    src/engine/RampProducer.h     src/engine/RampProducer.cpp
    src/engine/PitchLatency.h     src/engine/PitchLatency.cpp
    src/engine/Exporter.h         src/engine/Exporter.cpp
    src/engine/MediaCache.h       src/engine/MediaCache.cpp
    src/engine/StillFetcher.h     src/engine/StillFetcher.cpp
    src/engine/ProxyManager.h     src/engine/ProxyManager.cpp
    src/engine/RenderCache.h      src/engine/RenderCache.cpp
    src/engine/Preroll.h          src/engine/Preroll.cpp
    src/engine/AudioAnalysis.h    src/engine/AudioAnalysis.cpp
    src/engine/ColorGrade.h       src/engine/ColorGrade.cpp
    src/engine/Lumas.h            src/engine/Lumas.cpp
    src/engine/Frei0r.h           src/engine/Frei0r.cpp
    src/engine/RenderQueue.h      src/engine/RenderQueue.cpp

    src/app/Theme.h               src/app/Theme.cpp
    src/app/InputBindings.h       src/app/InputBindings.cpp
    src/app/MainWindow.h          src/app/MainWindow.cpp
    src/app/KeyBindingsDialog.h   src/app/KeyBindingsDialog.cpp
    src/app/ProjectSettingsDialog.h src/app/ProjectSettingsDialog.cpp
    src/app/ClipSpeedDialog.h src/app/ClipSpeedDialog.cpp
    src/app/NormalizeDialog.h src/app/NormalizeDialog.cpp
    src/app/PasteAttributesDialog.h src/app/PasteAttributesDialog.cpp
    src/app/DesignDialog.h src/app/DesignDialog.cpp
    src/app/Log.h                 src/app/Log.cpp

    src/ui/MediaPool.h            src/ui/MediaPool.cpp
    src/ui/EffectsLibrary.h       src/ui/EffectsLibrary.cpp
    src/ui/MediaStorage.h         src/ui/MediaStorage.cpp
    src/engine/Bundle.h           src/engine/Bundle.cpp
    src/ui/Viewer.h               src/ui/Viewer.cpp
    src/ui/TrimView.h             src/ui/TrimView.cpp
    src/ui/Inspector.h            src/ui/Inspector.cpp
    src/ui/ColorPanel.h           src/ui/ColorPanel.cpp
    src/ui/Scopes.h               src/ui/Scopes.cpp
    src/ui/ScopesPanel.h          src/ui/ScopesPanel.cpp
    src/ui/Mixer.h                src/ui/Mixer.cpp
    src/ui/ScrubField.h           src/ui/ScrubField.cpp
    src/ui/DeliverPanel.h         src/ui/DeliverPanel.cpp
    src/ui/RenderQueuePanel.h     src/ui/RenderQueuePanel.cpp
    src/ui/timeline/ViewState.h
    src/ui/timeline/TimelineView.h    src/ui/timeline/TimelineView.cpp
    src/ui/timeline/TimelineCurves.cpp
    src/ui/timeline/TimelinePanel.h   src/ui/timeline/TimelinePanel.cpp
)

qt_add_resources(schneidi_lib "i18n" PREFIX "/" FILES i18n/en.json)
set(SCHNEIDI_THEMES assets/themes/dark-orange.json assets/themes/midnight.json assets/themes/graphit.json assets/themes/hell.json assets/themes/beige.json)
qt_add_resources(schneidi_lib "themes" PREFIX "/" BASE assets FILES ${SCHNEIDI_THEMES})
file(GLOB SCHNEIDI_LUTS RELATIVE ${PROJECT_SOURCE_DIR} CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/assets/luts/*.cube) # tools/gen_luts.py
qt_add_resources(schneidi_lib "luts" PREFIX "/" BASE assets FILES ${SCHNEIDI_LUTS})
target_include_directories(schneidi_lib PUBLIC src)
target_link_libraries(schneidi_lib PUBLIC Qt6::Widgets PkgConfig::MLT)

add_executable(schneidi src/main.cpp)
target_include_directories(schneidi PRIVATE ${CMAKE_BINARY_DIR}/generated)
add_dependencies(schneidi schneidi_buildinfo)
target_link_libraries(schneidi PRIVATE schneidi_lib)

if(WIN32)
    set_target_properties(schneidi PROPERTIES WIN32_EXECUTABLE ON)
    target_sources(schneidi PRIVATE packaging/windows/schneidi.rc) # Programmsymbol
endif()

# Installation (Paketierung: AppImage unter Linux, später Programmordner unter Windows, siehe README.md)
include(GNUInstallDirs)
install(TARGETS schneidi RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(UNIX AND NOT APPLE)
    install(FILES packaging/linux/schneidi.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    install(FILES assets/schneidi.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
endif()

# Debian-Paket (schlank, Systembibliotheken statt Bundling): cmake --build build && cpack --config build/CPackConfig.cmake -G DEB
# Ausgabe nach dist/ (gitignored). Abhängigkeiten ermittelt dpkg-shlibdeps aus der Binärdatei.
if(UNIX AND NOT APPLE)
    set(CPACK_GENERATOR "DEB")
    set(CPACK_PACKAGE_NAME "schneidi")
    set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Simple video editor inspired by DaVinci Resolve")
    set(CPACK_PACKAGE_CONTACT "schneidi <noreply@example.com>")
    set(CPACK_PACKAGE_FILE_NAME "schneidi_${PROJECT_VERSION}_amd64")
    set(CPACK_STRIP_FILES ON)
    set(CPACK_PACKAGE_DIRECTORY "${CMAKE_BINARY_DIR}")
    set(CPACK_OUTPUT_FILE_PREFIX "${PROJECT_SOURCE_DIR}/dist")
    set(CPACK_DEBIAN_PACKAGE_SECTION "video")
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
    set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "amd64")
    # MLT modules (FFmpeg, frei0r, ...) are loaded at runtime, not linked; frei0r effects are optional
    set(CPACK_DEBIAN_PACKAGE_DEPENDS "libmlt-data, libmlt7")
    set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "frei0r-plugins, ffmpeg")
    include(CPack)
endif()

# Automatische Tests (CTest): cmake -DSCHNEIDI_TESTS=ON, siehe README.md
option(SCHNEIDI_TESTS "Tests bauen" OFF)
if(SCHNEIDI_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
