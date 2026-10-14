# Automatische Tests (CTest). Bauen/Ausführen:
#   cmake -S . -B build-tests -G Ninja -DSCHNEIDI_TESTS=ON && cmake --build build-tests
#   ctest --test-dir build-tests --output-on-failure
# Alle Tests laufen ohne sichtbare Fenster (offscreen) und ohne Ton (SDL dummy).

find_package(Qt6 REQUIRED COMPONENTS Widgets Test)

# Tests link against schneidi_lib from the top-level CMakeLists.txt (same sources as the app)

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
schneidi_test(test_i18n)
schneidi_test(test_effects_render)
schneidi_test(test_audio)
schneidi_test(test_mediapool)
schneidi_test(test_color)
schneidi_test(test_luts)
schneidi_test(test_lumas)
schneidi_test(test_frei0r)
schneidi_test(test_presets)
schneidi_test(test_rendercache)
schneidi_test(test_loudness)
schneidi_test(test_truepeak)
schneidi_test(test_limiter)
schneidi_test(test_render_queue)
schneidi_test(test_subtitles)
schneidi_test(test_liveupdate)
schneidi_test(test_transcript)
schneidi_test(test_sequences)
schneidi_test(test_retime)
schneidi_test(test_curves)
schneidi_test(test_theme)
schneidi_test(test_audio_streams)
schneidi_test(test_export_threads)
schneidi_test(test_export_formats)
schneidi_test(test_hw_encoders)
schneidi_test(test_export_opus)
schneidi_test(test_timeline_mouse)
schneidi_test(test_trimview)
schneidi_test(test_paste_attributes)
schneidi_test(test_scopes)
schneidi_test(test_stabilize_denoise)
schneidi_test(test_cli)
target_link_libraries(test_cli PRIVATE schneidi_cli_lib)
schneidi_test(test_live)
schneidi_test(test_transcript_cut)
target_link_libraries(test_live PRIVATE schneidi_cli_lib)
target_compile_definitions(test_live PRIVATE SCHNEIDI_CLI="$<TARGET_FILE:schneidi-cli>")
add_dependencies(test_live schneidi-cli)
schneidi_test(test_audio_regression)
schneidi_test(test_dynamics_eq)
target_link_libraries(test_dynamics_eq PRIVATE schneidi_cli_lib)

# Number format regressions (German decimal comma, e.g. 0.7.0: MLT reset LC_NUMERIC and stabilization did nothing):
# run tests that pass numbers to MLT/FFmpeg a second time under de_DE.UTF-8 (CI generates the locale)
foreach(name test_stabilize_denoise test_retime test_effects_render test_cli test_export_formats test_audio
             test_audio_regression test_color test_loudness test_dynamics_eq)
    add_test(NAME ${name}_de_DE COMMAND ${name})
    set_tests_properties(${name}_de_DE PROPERTIES
        SKIP_RETURN_CODE 77
        TIMEOUT 300
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;SDL_AUDIODRIVER=dummy;SDL_NO_SIGNAL_HANDLERS=1;LC_ALL=de_DE.UTF-8;LANG=de_DE.UTF-8")
endforeach()

# Benchmark, not run by CTest
add_executable(bench_inspector bench_inspector.cpp check.h)
target_link_libraries(bench_inspector PRIVATE schneidi_lib Qt6::Test)
add_executable(bench_engine bench_engine.cpp check.h)
target_link_libraries(bench_engine PRIVATE schneidi_lib Qt6::Test)
