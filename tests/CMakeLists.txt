# Automatische Tests (CTest). Bauen/Ausführen:
#   cmake -S . -B build-tests -G Ninja -DSCHNEIDI_TESTS=ON && cmake --build build-tests
#   ctest --test-dir build-tests --output-on-failure
# Alle Tests laufen ohne sichtbare Fenster (offscreen) und ohne Ton (SDL dummy).

find_package(Qt6 REQUIRED COMPONENTS Widgets Test)

# Alle Programmquellen außer main.cpp als statische Bibliothek, die sich alle Testprogramme teilen
file(GLOB_RECURSE SCHNEIDI_LIB_SOURCES CONFIGURE_DEPENDS
    ${PROJECT_SOURCE_DIR}/src/*.cpp ${PROJECT_SOURCE_DIR}/src/*.h)
list(FILTER SCHNEIDI_LIB_SOURCES EXCLUDE REGEX ".*/src/main\\.cpp$")

add_library(schneidi_lib STATIC ${SCHNEIDI_LIB_SOURCES})
set_target_properties(schneidi_lib PROPERTIES AUTOMOC ON)
qt_add_resources(schneidi_lib "i18n" PREFIX "/" BASE ${PROJECT_SOURCE_DIR} FILES ${PROJECT_SOURCE_DIR}/i18n/en.json)
list(TRANSFORM SCHNEIDI_THEMES PREPEND ${PROJECT_SOURCE_DIR}/)
qt_add_resources(schneidi_lib "themes" PREFIX "/" BASE ${PROJECT_SOURCE_DIR}/assets FILES ${SCHNEIDI_THEMES})
target_include_directories(schneidi_lib PUBLIC ${PROJECT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(schneidi_lib PUBLIC Qt6::Widgets PkgConfig::MLT)

# Rückgabecode 77 = übersprungen (z. B. ffmpeg fehlt)
function(schneidi_test name)
    add_executable(${name} ${name}.cpp check.h)
    target_link_libraries(${name} PRIVATE schneidi_lib Qt6::Test)
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES
        SKIP_RETURN_CODE 77
        TIMEOUT 300
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;SDL_AUDIODRIVER=dummy;SDL_NO_SIGNAL_HANDLERS=1")
endfunction()

schneidi_test(test_timelineops)
schneidi_test(test_preroll)
schneidi_test(test_projectfile)
schneidi_test(test_undo)
schneidi_test(test_tracks)
schneidi_test(test_source_edits)
schneidi_test(test_effects)
schneidi_test(test_effects_render)
schneidi_test(test_audio)
schneidi_test(test_mediapool)
schneidi_test(test_color)
schneidi_test(test_rendercache)
schneidi_test(test_loudness)
schneidi_test(test_render_queue)
schneidi_test(test_subtitles)
schneidi_test(test_sequences)
schneidi_test(test_retime)
schneidi_test(test_curves)
schneidi_test(test_theme)
schneidi_test(test_audio_streams)
schneidi_test(test_export_threads)
schneidi_test(test_export_formats)
