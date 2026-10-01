cmake_minimum_required(VERSION 3.21)
project(schneidi VERSION 0.3.0 LANGUAGES CXX)

# Ohne Angabe optimiert bauen (Debug-Builds ruckeln bei der Wiedergabe: eigene Filter laufen pro Frame)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build-Typ" FORCE)
endif()

set(CMAKE_CXX_STANDARD 17)
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

add_executable(schneidi
    src/main.cpp

    src/core/Types.h
    src/core/I18n.h               src/core/I18n.cpp
    src/core/Timecode.h
    src/core/TimelineOps.h        src/core/TimelineOps.cpp
    src/core/Keyframes.h          src/core/Keyframes.cpp
    src/core/Retime.h             src/core/Retime.cpp
    src/core/ProjectFormat.h      src/core/ProjectFormat.cpp
    src/core/Project.h            src/core/Project.cpp
    src/core/ProjectFile.h        src/core/ProjectFile.cpp
    src/core/Selection.h
    src/core/Editor.h             src/core/Editor.cpp
    src/core/EditorSubtitles.cpp
    src/core/EditorCompound.cpp
    src/core/EffectRegistry.h     src/core/EffectRegistry.cpp
    src/core/Loudness.h           src/core/Loudness.cpp
    src/core/RenderJob.h          src/core/RenderJob.cpp
    src/core/Subtitles.h          src/core/Subtitles.cpp

    src/engine/Engine.h           src/engine/Engine.cpp
    src/engine/Profiles.h         src/engine/Profiles.cpp
    src/engine/TimelineBuilder.h  src/engine/TimelineBuilder.cpp
    src/engine/RampProducer.h     src/engine/RampProducer.cpp
    src/engine/PitchLatency.h     src/engine/PitchLatency.cpp
    src/engine/Exporter.h         src/engine/Exporter.cpp
    src/engine/MediaCache.h       src/engine/MediaCache.cpp
    src/engine/ProxyManager.h     src/engine/ProxyManager.cpp
    src/engine/RenderCache.h      src/engine/RenderCache.cpp
    src/engine/Preroll.h          src/engine/Preroll.cpp
    src/engine/AudioAnalysis.h    src/engine/AudioAnalysis.cpp
    src/engine/ColorGrade.h       src/engine/ColorGrade.cpp
    src/engine/RenderQueue.h      src/engine/RenderQueue.cpp

    src/app/Theme.h               src/app/Theme.cpp
    src/app/InputBindings.h       src/app/InputBindings.cpp
    src/app/MainWindow.h          src/app/MainWindow.cpp
    src/app/KeyBindingsDialog.h   src/app/KeyBindingsDialog.cpp
    src/app/ProjectSettingsDialog.h src/app/ProjectSettingsDialog.cpp
    src/app/ClipSpeedDialog.h src/app/ClipSpeedDialog.cpp
    src/app/NormalizeDialog.h src/app/NormalizeDialog.cpp
    src/app/DesignDialog.h src/app/DesignDialog.cpp
    src/app/Log.h                 src/app/Log.cpp

    src/ui/MediaPool.h            src/ui/MediaPool.cpp
    src/ui/EffectsLibrary.h       src/ui/EffectsLibrary.cpp
    src/ui/MediaStorage.h         src/ui/MediaStorage.cpp
    src/engine/Bundle.h           src/engine/Bundle.cpp
    src/ui/Viewer.h               src/ui/Viewer.cpp
    src/ui/Inspector.h            src/ui/Inspector.cpp
    src/ui/ColorPanel.h           src/ui/ColorPanel.cpp
    src/ui/Mixer.h                src/ui/Mixer.cpp
    src/ui/ScrubField.h           src/ui/ScrubField.cpp
    src/ui/DeliverPanel.h         src/ui/DeliverPanel.cpp
    src/ui/RenderQueuePanel.h     src/ui/RenderQueuePanel.cpp
    src/ui/timeline/ViewState.h
    src/ui/timeline/TimelineView.h    src/ui/timeline/TimelineView.cpp
    src/ui/timeline/TimelineCurves.cpp
    src/ui/timeline/TimelinePanel.h   src/ui/timeline/TimelinePanel.cpp
)

qt_add_resources(schneidi "i18n" PREFIX "/" FILES i18n/en.json)
set(SCHNEIDI_THEMES assets/themes/dark-orange.json assets/themes/midnight.json assets/themes/graphit.json assets/themes/hell.json)
qt_add_resources(schneidi "themes" PREFIX "/" BASE assets FILES ${SCHNEIDI_THEMES})

target_include_directories(schneidi PRIVATE src ${CMAKE_BINARY_DIR}/generated)
add_dependencies(schneidi schneidi_buildinfo)
target_link_libraries(schneidi PRIVATE Qt6::Widgets PkgConfig::MLT)

if(WIN32)
    set_target_properties(schneidi PROPERTIES WIN32_EXECUTABLE ON)
    target_sources(schneidi PRIVATE packaging/windows/schneidi.rc) # Programmsymbol
endif()

# Installation (Paketierung: AppImage unter Linux, später Programmordner unter Windows, siehe docs/dev-notes.md)
include(GNUInstallDirs)
install(TARGETS schneidi RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(UNIX AND NOT APPLE)
    install(FILES packaging/linux/schneidi.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    install(FILES assets/schneidi.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
endif()

# Automatische Tests (CTest): cmake -DSCHNEIDI_TESTS=ON, siehe docs/dev-notes.md
option(SCHNEIDI_TESTS "Tests bauen" OFF)
if(SCHNEIDI_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
