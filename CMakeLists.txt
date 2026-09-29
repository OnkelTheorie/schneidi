cmake_minimum_required(VERSION 3.21)
project(schneidi VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets)
find_package(PkgConfig REQUIRED)
# MLT liefert unter Linux und MSYS2 (Windows) eine .pc-Datei
pkg_check_modules(MLT REQUIRED IMPORTED_TARGET mlt++-7)

add_executable(schneidi
    src/main.cpp

    src/core/Types.h
    src/core/I18n.h               src/core/I18n.cpp
    src/core/Timecode.h
    src/core/TimelineOps.h        src/core/TimelineOps.cpp
    src/core/Keyframes.h          src/core/Keyframes.cpp
    src/core/ProjectFormat.h      src/core/ProjectFormat.cpp
    src/core/Project.h            src/core/Project.cpp
    src/core/ProjectFile.h        src/core/ProjectFile.cpp
    src/core/Selection.h
    src/core/Editor.h             src/core/Editor.cpp
    src/core/EffectRegistry.h     src/core/EffectRegistry.cpp
    src/core/Loudness.h           src/core/Loudness.cpp
    src/core/RenderJob.h          src/core/RenderJob.cpp

    src/engine/Engine.h           src/engine/Engine.cpp
    src/engine/Profiles.h         src/engine/Profiles.cpp
    src/engine/TimelineBuilder.h  src/engine/TimelineBuilder.cpp
    src/engine/Exporter.h         src/engine/Exporter.cpp
    src/engine/MediaCache.h       src/engine/MediaCache.cpp
    src/engine/ProxyManager.h     src/engine/ProxyManager.cpp
    src/engine/AudioAnalysis.h    src/engine/AudioAnalysis.cpp

    src/app/Theme.h               src/app/Theme.cpp
    src/app/InputBindings.h       src/app/InputBindings.cpp
    src/app/MainWindow.h          src/app/MainWindow.cpp
    src/app/KeyBindingsDialog.h   src/app/KeyBindingsDialog.cpp
    src/app/ProjectSettingsDialog.h src/app/ProjectSettingsDialog.cpp
    src/app/ClipSpeedDialog.h src/app/ClipSpeedDialog.cpp
    src/app/NormalizeDialog.h src/app/NormalizeDialog.cpp

    src/ui/MediaPool.h            src/ui/MediaPool.cpp
    src/ui/EffectsLibrary.h       src/ui/EffectsLibrary.cpp
    src/ui/MediaStorage.h         src/ui/MediaStorage.cpp
    src/engine/Bundle.h           src/engine/Bundle.cpp
    src/ui/Viewer.h               src/ui/Viewer.cpp
    src/ui/Inspector.h            src/ui/Inspector.cpp
    src/ui/Mixer.h                src/ui/Mixer.cpp
    src/ui/ScrubField.h           src/ui/ScrubField.cpp
    src/ui/DeliverPanel.h         src/ui/DeliverPanel.cpp
    src/ui/timeline/ViewState.h
    src/ui/timeline/TimelineView.h    src/ui/timeline/TimelineView.cpp
    src/ui/timeline/TimelinePanel.h   src/ui/timeline/TimelinePanel.cpp
)

qt_add_resources(schneidi "i18n" PREFIX "/" FILES i18n/en.json)

target_include_directories(schneidi PRIVATE src)
target_link_libraries(schneidi PRIVATE Qt6::Widgets PkgConfig::MLT)

if(WIN32)
    set_target_properties(schneidi PROPERTIES WIN32_EXECUTABLE ON)
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
