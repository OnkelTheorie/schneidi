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
    # RESOURCE_LOCK: the locale/display variants below run the same program with the same test folders – never at
    # the same time as it (test_luts_c_locale deleted a folder test_luts was looking for)
    set_tests_properties(${name} PROPERTIES
        SKIP_RETURN_CODE 77
        TIMEOUT 300
        RESOURCE_LOCK ${name}
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;SDL_AUDIODRIVER=dummy;SDL_NO_SIGNAL_HANDLERS=1")
endfunction()

schneidi_test(test_timelineops)
schneidi_test(test_interchange)
target_compile_definitions(test_interchange PRIVATE SCHNEIDI_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/data")
schneidi_test(test_interchange_app)
schneidi_test(test_preroll)
schneidi_test(test_projectfile)
schneidi_test(test_project_versions)
target_compile_definitions(test_project_versions PRIVATE SCHNEIDI_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/data")
schneidi_test(test_undo)
schneidi_test(test_tracks)
schneidi_test(test_source_edits)
schneidi_test(test_effects)
schneidi_test(test_i18n)
schneidi_test(test_effects_render)
schneidi_test(test_output_color)
schneidi_test(test_preview_export)
schneidi_test(test_audio)
schneidi_test(test_mlt_threads)
schneidi_test(test_preview_audio)
# plays in real time: under parallel load the preview could fall behind (gaps in the sound = false clicks)
set_tests_properties(test_preview_audio PROPERTIES RUN_SERIAL TRUE)
schneidi_test(test_mediapool)
schneidi_test(test_color)
schneidi_test(test_grade_windows)
schneidi_test(test_workspace)
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
# Reference renders of the sound (own audio engine, docs/plan-0.10-ton.md): references in the private docs folder
schneidi_test(test_audio_reference)
schneidi_test(test_av_sync)
schneidi_test(test_audio_reader)
schneidi_test(test_audio_mixer)
schneidi_test(test_export_audio)
schneidi_test(test_audio_analysis)
target_compile_definitions(test_audio_analysis PRIVATE SCHNEIDI_AUDIO_REFERENCE="${CMAKE_SOURCE_DIR}/docs/testdaten/ton-referenz")
target_compile_definitions(test_export_audio PRIVATE SCHNEIDI_AUDIO_REFERENCE="${CMAKE_SOURCE_DIR}/docs/testdaten/ton-referenz")
target_compile_definitions(test_audio_mixer PRIVATE SCHNEIDI_AUDIO_REFERENCE="${CMAKE_SOURCE_DIR}/docs/testdaten/ton-referenz")
set_tests_properties(test_av_sync PROPERTIES RUN_SERIAL TRUE) # preview timing
target_compile_definitions(test_audio_reference PRIVATE SCHNEIDI_AUDIO_REFERENCE="${CMAKE_SOURCE_DIR}/docs/testdaten/ton-referenz")
schneidi_test(test_dynamics_eq)
target_link_libraries(test_dynamics_eq PRIVATE schneidi_cli_lib)
schneidi_test(test_audio_sync)
schneidi_test(test_multicam)
schneidi_test(test_multicam_render)
schneidi_test(test_multicam_viewer)
schneidi_test(test_multicam_cli)
target_link_libraries(test_multicam_cli PRIVATE schneidi_cli_lib)
schneidi_test(test_stream_presets)
target_link_libraries(test_stream_presets PRIVATE schneidi_cli_lib)
schneidi_test(test_ducking)
target_link_libraries(test_ducking PRIVATE schneidi_cli_lib)
# Speed brake: alone (parallel tests would distort the times), baseline per machine in data/speed-baseline.json
schneidi_test(test_speed)
target_compile_definitions(test_speed PRIVATE SCHNEIDI_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/data")
set_tests_properties(test_speed PROPERTIES RUN_SERIAL TRUE TIMEOUT 600)

# Number format regressions (German decimal comma, e.g. 0.7.0: MLT reset LC_NUMERIC and stabilization did nothing):
# run tests that pass numbers to MLT/FFmpeg a second time under de_DE.UTF-8 (CI generates the locale)
foreach(name test_stabilize_denoise test_retime test_effects_render test_cli test_export_formats test_audio
             test_audio_regression test_color test_loudness test_dynamics_eq test_audio_sync test_multicam_render
             test_multicam_viewer test_multicam_cli
             test_ducking)
    add_test(NAME ${name}_de_DE COMMAND ${name})
    set_tests_properties(${name}_de_DE PROPERTIES
        SKIP_RETURN_CODE 77
        TIMEOUT 300
        RESOURCE_LOCK ${name}
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;SDL_AUDIODRIVER=dummy;SDL_NO_SIGNAL_HANDLERS=1;LC_ALL=de_DE.UTF-8;LANG=de_DE.UTF-8")
endforeach()

# Own audio engine (docs/plan-0.10-ton.md): export and analysis tests a second time with the sound from Audio::Mixer
# instead of MLT (the default until the switch-over); known MLT errors become strict checks there
foreach(name test_av_sync test_audio_regression test_export_formats test_audio_streams test_export_opus test_cli
             test_multicam_render test_render_queue test_loudness test_ducking test_audio_sync test_dynamics_eq
             test_multicam_cli)
    add_test(NAME ${name}_own_audio COMMAND ${name})
    set_tests_properties(${name}_own_audio PROPERTIES
        SKIP_RETURN_CODE 77
        TIMEOUT 300
        RESOURCE_LOCK ${name}
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;SDL_AUDIODRIVER=dummy;SDL_NO_SIGNAL_HANDLERS=1;SCHNEIDI_AUDIO=own")
endforeach()

# Server/container without a display: MLT's Qt module (qtblend, qtext) must still load – it used to refuse without
# $DISPLAY and the timeline crashed on the invalid transition (see Bundle::prepareMltEnvironment)
foreach(name test_cli test_multicam_render)
    add_test(NAME ${name}_no_display COMMAND ${name})
    set_tests_properties(${name}_no_display PROPERTIES
        SKIP_RETURN_CODE 77
        RESOURCE_LOCK ${name}
        TIMEOUT 300
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;SDL_AUDIODRIVER=dummy;SDL_NO_SIGNAL_HANDLERS=1"
        ENVIRONMENT_MODIFICATION "DISPLAY=unset:;WAYLAND_DISPLAY=unset:")
endforeach()

# C/POSIX locale (servers, containers): name lists must still sort without case first
add_test(NAME test_luts_c_locale COMMAND test_luts)
set_tests_properties(test_luts_c_locale PROPERTIES
    SKIP_RETURN_CODE 77
    RESOURCE_LOCK test_luts
    TIMEOUT 300
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;SDL_AUDIODRIVER=dummy;SDL_NO_SIGNAL_HANDLERS=1;LC_ALL=C;LANG=C")

# Benchmark, not run by CTest
add_executable(bench_inspector bench_inspector.cpp check.h)
target_link_libraries(bench_inspector PRIVATE schneidi_lib Qt6::Test)
add_executable(bench_engine bench_engine.cpp check.h)
target_link_libraries(bench_engine PRIVATE schneidi_lib Qt6::Test)
add_executable(bench_playback bench_playback.cpp check.h)
target_link_libraries(bench_playback PRIVATE schneidi_lib Qt6::Test)
add_executable(bench_multicam bench_multicam.cpp check.h)
target_link_libraries(bench_multicam PRIVATE schneidi_lib Qt6::Test)
add_executable(bench_export bench_export.cpp check.h)
target_link_libraries(bench_export PRIVATE schneidi_lib Qt6::Test)
add_executable(bench_undo bench_undo.cpp check.h)
target_link_libraries(bench_undo PRIVATE schneidi_lib Qt6::Test)
add_executable(bench_audio_reader bench_audio_reader.cpp check.h)
target_link_libraries(bench_audio_reader PRIVATE schneidi_lib Qt6::Test)
