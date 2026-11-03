# Automatische Tests (CTest). Bauen/Ausführen:
#   cmake -S . -B build-tests -G Ninja -DSCHNEIDI_TESTS=ON && cmake --build build-tests
#   ctest --test-dir build-tests --output-on-failure
# Alle Tests laufen ohne sichtbare Fenster (offscreen) und ohne Ton (eigenes Gerät "null").

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
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;SCHNEIDI_AUDIO_DEVICE=null;SCHNEIDI_GPU=0")
endfunction()

schneidi_test(test_timelineops)
schneidi_test(test_interchange)
target_compile_definitions(test_interchange PRIVATE SCHNEIDI_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/data")
schneidi_test(test_interchange_app)
schneidi_test(test_projectfile)
schneidi_test(test_generators)
target_link_libraries(test_generators PRIVATE schneidi_cli_lib)
schneidi_test(test_project_versions)
target_compile_definitions(test_project_versions PRIVATE SCHNEIDI_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/data")
schneidi_test(test_undo)
schneidi_test(test_tracks)
schneidi_test(test_source_edits)
schneidi_test(test_effects)
# Effect order: Editor::moveEffect, op 'effect' move, Inspector (menu, dragging the header), render follows the order
schneidi_test(test_effect_order)
target_link_libraries(test_effect_order PRIVATE schneidi_cli_lib)
schneidi_test(test_i18n)
schneidi_test(test_effects_render)
schneidi_test(test_output_color) # RGBA -> encoder formats (all layouts, 8/10 bit, NV12/P010, j formats) against the formula
schneidi_test(test_preview_export)
schneidi_test(test_audio)
schneidi_test(test_preview_audio)
schneidi_test(test_voiceover) # voice-over: sound input into a WAV (stand-in input), lining up, placing on the armed track
schneidi_test(test_voiceover_app) # voice-over in the app: arm, record button, stop by playback, take on the track, undo
# plays in real time: under parallel load the preview could fall behind (gaps in the sound = false clicks)
set_tests_properties(test_preview_audio PROPERTIES RUN_SERIAL TRUE)
schneidi_test(test_mediapool)
schneidi_test(test_pool_storage) # Media Pool filter + keys, Media Storage marks, hover scrub, look before import
schneidi_test(test_pool_remove) # removing clips from the Media Pool: core op, undo, watch, AI turn, CLI op, keys, question
target_link_libraries(test_pool_remove PRIVATE schneidi_cli_lib)
schneidi_test(test_color)
schneidi_test(test_color_bake)
schneidi_test(test_grade_windows)
schneidi_test(test_workspace)
schneidi_test(test_luts)
schneidi_test(test_lumas)
schneidi_test(test_key_pick)
schneidi_test(test_viewer_large) # large viewer: panels hide and come back exactly, timeline zoom/scroll untouched
schneidi_test(test_frame_view) # viewer picture widget: GPU/painter choice, zoom/pan, overlay mapping, zoom actions
schneidi_test(test_picture_gpu) # picture engine on the GPU vs the CPU path (skips without a hardware device)
schneidi_test(test_launch_args) # app command line: options (also Qt's X11 -geometry) are no files to import
schneidi_test(test_picture_gpu_render) # Renderer's GPU path vs its CPU path, whole frames (skips without a GPU)
# The GPU decides by itself here (every other test runs the CPU path: SCHNEIDI_GPU=0)
set_tests_properties(test_picture_gpu_render PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;SCHNEIDI_AUDIO_DEVICE=null")
schneidi_test(test_own_effects)
schneidi_test(test_presets)
schneidi_test(test_rendercache)
schneidi_test(test_loudness)
schneidi_test(test_truepeak)
schneidi_test(test_limiter)
schneidi_test(test_render_queue)
schneidi_test(test_subtitles)
schneidi_test(test_transcript)
schneidi_test(test_sequences)
schneidi_test(test_retime)
schneidi_test(test_interpolate) # slow motion in-between pictures: blend, motion (flow), HUD, cuts
schneidi_test(test_curves)
schneidi_test(test_theme)
schneidi_test(test_audio_streams)
schneidi_test(test_export_threads)
schneidi_test(test_export_formats)
schneidi_test(test_export_writer)
schneidi_test(test_export_timing)
schneidi_test(test_smart_render)
schneidi_test(test_filmstrips) # filmstrips + Media Storage previews with the own decoder: same pictures as the ffmpeg command
schneidi_test(test_occlusion)
schneidi_test(test_picture_passthrough) # export: a plain full-size video hands its decoded planes to the encoder; full-frame stills at the bottom
schneidi_test(test_video_convert) # toRgba sets the colour matrix per conversion (BT.2020 after/before 601)
schneidi_test(test_video_reader) # own decoder: jumps/threads/hardware = pictures in order, hibernation, rotation, file properties
schneidi_test(test_picture_regression) # picture engine against independent references: timing, layers, transitions, outline, downscale, threads
schneidi_test(test_crop)
schneidi_test(test_picture_pump) # preview frame pump: sync under load, catch-up, seek, live change, backward
schneidi_test(test_fast_seek) # seek window, scrubbing (close picture, then the exact one), recently shown frames
schneidi_test(test_hw_encoders)
schneidi_test(test_export_opus)
schneidi_test(test_license_free)
schneidi_test(test_encoder_extension)
# a failed/canceled export keeps an older file of the same name, write errors midway fail, odd project sizes
schneidi_test(test_export_safety)
schneidi_test(test_timeline_mouse)
schneidi_test(test_timeline_keys) # key bindings of the window (no clashes per set), slip/slide tools, lock, transcript keys
schneidi_test(test_trimview)
schneidi_test(test_paste_attributes)
schneidi_test(test_scopes)
schneidi_test(test_graded_thumbs)
schneidi_test(test_stabilize_denoise)
# Own stabilizer core: known camera motion recovered (moving object, still overlay), smoothed path, warp speed
schneidi_test(test_stabilize_motion)
schneidi_test(test_cli)
target_link_libraries(test_cli PRIVATE schneidi_cli_lib)
schneidi_test(test_live)
schneidi_test(test_transcribe_pick) # which audio stream/track transcribe hears (a fake whisper-cli)
target_link_libraries(test_transcribe_pick PRIVATE schneidi_cli_lib)
target_compile_definitions(test_transcribe_pick PRIVATE SCHNEIDI_CLI="$<TARGET_FILE:schneidi-cli>")
add_dependencies(test_transcribe_pick schneidi-cli)
schneidi_test(test_transcript_cut)
schneidi_test(test_captions) # word captions: timing, edits, CLI op, render, embedded text streams
target_link_libraries(test_captions PRIVATE schneidi_cli_lib)
schneidi_test(test_short) # `short`: moments, Shorts/Reels/TikTok presets, check platform, limiter, end to end
target_link_libraries(test_short PRIVATE schneidi_cli_lib)
schneidi_test(test_speech) # text to speech: piper extension, cache, fake piper, CLI op speak
target_link_libraries(test_speech PRIVATE schneidi_cli_lib)
target_link_libraries(test_live PRIVATE schneidi_cli_lib)
target_compile_definitions(test_live PRIVATE SCHNEIDI_CLI="$<TARGET_FILE:schneidi-cli>")
add_dependencies(test_live schneidi-cli)
# The 0.16 edit ops live in the app: one undo step each, undo exact, same result as on the file
schneidi_test(test_live_ops016)
target_link_libraries(test_live_ops016 PRIVATE schneidi_cli_lib)
target_compile_definitions(test_live_ops016 PRIVATE SCHNEIDI_CLI="$<TARGET_FILE:schneidi-cli>")
add_dependencies(test_live_ops016 schneidi-cli)
# Files handed to the running app (single instance over the live connection), videos opened as projects, `app open`,
# the real program offscreen
schneidi_test(test_open_in_app)
target_link_libraries(test_open_in_app PRIVATE schneidi_cli_lib)
target_compile_definitions(test_open_in_app PRIVATE SCHNEIDI_CLI="$<TARGET_FILE:schneidi-cli>"
                           SCHNEIDI_APP="$<TARGET_FILE:schneidi>")
add_dependencies(test_open_in_app schneidi-cli schneidi)
# Launches with files while an app runs: a slow or busy app still gets them, two launches at once end in one app
schneidi_test(test_handover)
target_link_libraries(test_handover PRIVATE schneidi_cli_lib)
schneidi_test(test_setup)
schneidi_test(test_check)
schneidi_test(test_contact_sheet)
schneidi_test(test_overview)
target_link_libraries(test_overview PRIVATE schneidi_cli_lib)
schneidi_test(test_analysis_scan) # scenes/overview/region/colour events in-process == the ffmpeg way
target_link_libraries(test_analysis_scan PRIVATE schneidi_cli_lib)
schneidi_test(test_beats) # `beats`: tempo, bars, phrases, breaks, drops of a song
target_link_libraries(test_beats PRIVATE schneidi_cli_lib)
schneidi_test(test_audio_events) # sound events (heuristics): speech, music, silence, laughter, applause, cheering, shots, screams; cache; peaks {events}
target_link_libraries(test_audio_events PRIVATE schneidi_cli_lib)
target_link_libraries(test_check PRIVATE schneidi_cli_lib)
target_link_libraries(test_setup PRIVATE schneidi_cli_lib)
target_compile_definitions(test_setup PRIVATE SCHNEIDI_CLI="$<TARGET_FILE:schneidi-cli>")
add_dependencies(test_setup schneidi-cli)
# MCP over HTTP: real server processes (workers are schneidi-cli mcp) on a free port
schneidi_test(test_serve)
target_link_libraries(test_serve PRIVATE schneidi_cli_lib)
target_compile_definitions(test_serve PRIVATE SCHNEIDI_CLI="$<TARGET_FILE:schneidi-cli>")
add_dependencies(test_serve schneidi-cli)
schneidi_test(test_audio_regression)
# Reference renders of the sound (own audio engine, docs/archiv/plan-0.10-ton.md): references in the private docs folder
schneidi_test(test_audio_reference)
schneidi_test(test_av_sync)
schneidi_test(test_audio_reader)
schneidi_test(test_audio_reader_damage) # vanishing files, timestamp gaps and outliers, Matroska without duration
schneidi_test(test_media_probe)
schneidi_test(test_audio_mixer)
schneidi_test(test_stretch) # own time stretcher (keep pitch)
schneidi_test(test_export_audio)
schneidi_test(test_audio_analysis)
schneidi_test(test_audio_player)
set_tests_properties(test_audio_player PROPERTIES RUN_SERIAL TRUE) # real time
schneidi_test(test_play_end) # playback stops at the end and stays stopped
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
schneidi_test(test_proxy_hint)
schneidi_test(test_media_timing) # proxy frame k = original frame k, shorter sources keep later clips in place, frame positions
schneidi_test(test_proxy_cleanup) # which proxy files cleaning up deletes, which stay, dry run
schneidi_test(test_memory_release) # picture pools and the heap given back while idle, never during playback
schneidi_test(test_settings) # app settings (File › Settings…), AI page, indicator modes
schneidi_test(test_update_check) # update hint against a local HTTP server
schneidi_test(test_ai_activity) # AI changes light up, AI log, indicator with the AI's work
schneidi_test(test_ai_providers) # AI chat: requests/answers of both protocols, keys, company file (no network)
# AI chat end to end: MainWindow + live server, the real schneidi-cli mcp, a fake provider on 127.0.0.1
schneidi_test(test_ai_chat)
target_link_libraries(test_ai_chat PRIVATE schneidi_cli_lib)
target_compile_definitions(test_ai_chat PRIVATE SCHNEIDI_CLI="$<TARGET_FILE:schneidi-cli>")
add_dependencies(test_ai_chat schneidi-cli)
schneidi_test(test_multicam_cli)
target_link_libraries(test_multicam_cli PRIVATE schneidi_cli_lib)
schneidi_test(test_stream_presets)
target_link_libraries(test_stream_presets PRIVATE schneidi_cli_lib)
schneidi_test(test_start_templates) # how new projects look: start templates (app, settings page, CLI new/templates)
target_link_libraries(test_start_templates PRIVATE schneidi_cli_lib)
schneidi_test(test_ducking)
target_link_libraries(test_ducking PRIVATE schneidi_cli_lib)
schneidi_test(test_blur_fill) # blurred fill around the fitted picture (op 'transform' fit:'blur')
target_link_libraries(test_blur_fill PRIVATE schneidi_cli_lib)
schneidi_test(test_reframe) # Auto-Reframe: camera path, fill geometry, Editor op + undo, subject measurement, CLI ops
schneidi_test(test_reframe_keep) # Auto-Reframe keeps the kill feed (picture in picture at the top), game kill feed regions in highlights
target_link_libraries(test_reframe_keep PRIVATE schneidi_cli_lib)
schneidi_test(test_game43) # 4:3 game footage: black bar detection, reframe inside the content, crop_bars, stretched 4:3 (picture aspect), CLI ops
target_link_libraries(test_game43 PRIVATE schneidi_cli_lib)
target_link_libraries(test_reframe PRIVATE schneidi_cli_lib)
schneidi_test(test_name_blur) # hide player names: text regions, templates, masks over time, Editor op, CLI op blur_names
target_link_libraries(test_name_blur PRIVATE schneidi_cli_lib)
schneidi_test(test_accents) # zoom punch / flash / shake as keyframes (op 'accent', clip menu), param zoom
target_link_libraries(test_accents PRIVATE schneidi_cli_lib)
# Speed brake: alone (parallel tests would distort the times), baseline per machine in data/speed-baseline.json
# Watch folders: rules, project, `watch` command, schneidi-cli (--follow with a file written in chunks), live, app watcher
schneidi_test(test_watch_folders)
target_link_libraries(test_watch_folders PRIVATE schneidi_cli_lib)
target_compile_definitions(test_watch_folders PRIVATE SCHNEIDI_CLI="$<TARGET_FILE:schneidi-cli>"
                           SCHNEIDI_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/data")
add_dependencies(test_watch_folders schneidi-cli)
schneidi_test(test_speed)
target_compile_definitions(test_speed PRIVATE SCHNEIDI_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/data")
set_tests_properties(test_speed PROPERTIES RUN_SERIAL TRUE TIMEOUT 600)
schneidi_test(test_file_manager) # show in file manager / open with the default program, also without a D-Bus session
# Background workers stopped/reset/canceled while busy (ScrubFrames, GradedThumbs, FolderWatch, render cache/queue):
# a stress test for the thread sanitizer and ASan, a few seconds in the normal suite
schneidi_test(test_thread_teardown)

# Number format regressions (German decimal comma, e.g. 0.7.0: stabilization did nothing): run tests that pass numbers
# to FFmpeg or parse them a second time under de_DE.UTF-8 (CI generates the locale)
foreach(name test_stabilize_denoise test_retime test_effects_render test_effect_order test_cli test_export_formats test_audio
             test_audio_regression test_color test_loudness test_dynamics_eq test_audio_sync test_multicam_render
             test_multicam_viewer test_multicam_cli
             test_ducking test_crop test_blur_fill test_reframe test_reframe_keep test_game43 test_generators)
    add_test(NAME ${name}_de_DE COMMAND ${name})
    set_tests_properties(${name}_de_DE PROPERTIES
        SKIP_RETURN_CODE 77
        TIMEOUT 300
        RESOURCE_LOCK ${name}
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;SCHNEIDI_AUDIO_DEVICE=null;SCHNEIDI_GPU=0;LC_ALL=de_DE.UTF-8;LANG=de_DE.UTF-8")
endforeach()

set_tests_properties(test_av_sync test_preview_audio test_play_end PROPERTIES RUN_SERIAL TRUE) # preview timing

# Label "timing": tests that measure real time or wait for other processes. They fail when the machine is busy (other
# builds, a game, a parallel ctest), not when the code is wrong. tools/run_tests.py runs everything else in parallel,
# then these one by one, and once more what failed (ctest -LE timing / -L timing work by hand too).
set_tests_properties(test_av_sync test_preview_audio test_play_end test_audio_player test_speed test_serve
                     test_watch_folders test_voiceover test_voiceover_app test_handover
                     PROPERTIES LABELS timing)

# Server/container without a display: titles (Qt's fonts) and the renderer still work
foreach(name test_cli test_multicam_render)
    add_test(NAME ${name}_no_display COMMAND ${name})
    set_tests_properties(${name}_no_display PROPERTIES
        SKIP_RETURN_CODE 77
        RESOURCE_LOCK ${name}
        TIMEOUT 300
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;SCHNEIDI_AUDIO_DEVICE=null;SCHNEIDI_GPU=0"
        ENVIRONMENT_MODIFICATION "DISPLAY=unset:;WAYLAND_DISPLAY=unset:")
endforeach()

# C/POSIX locale (servers, containers): name lists must still sort without case first
add_test(NAME test_luts_c_locale COMMAND test_luts)
set_tests_properties(test_luts_c_locale PROPERTIES
    SKIP_RETURN_CODE 77
    RESOURCE_LOCK test_luts
    TIMEOUT 300
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;SCHNEIDI_AUDIO_DEVICE=null;SCHNEIDI_GPU=0;LC_ALL=C;LANG=C")

# HTTP side of the server on a socket without a network (fuzz/FakeSocket.h): pipelining, dropped bodies, Range
schneidi_test(test_http)
target_link_libraries(test_http PRIVATE schneidi_cli_lib)

# Fuzz targets (tests/fuzz) without libFuzzer: each one's code with its rules on its seeds and on every input that
# once found a bug (fuzz/regressions/<target>: put the shrunk input of a finding there, next to the fix)
set(fuzz_format_edl Edl)
set(fuzz_format_fcp7 Fcp7)
set(fuzz_format_otio Otio)
foreach(target http mcp project srt edl fcp7 otio)
    if(DEFINED fuzz_format_${target}) # one source for the three exchange formats
        add_executable(replay_${target} fuzz/fuzz_interchange.cpp fuzz/replay_main.cpp fuzz/FuzzCommon.h)
        target_compile_definitions(replay_${target} PRIVATE FUZZ_FORMAT=${fuzz_format_${target}})
    else()
        add_executable(replay_${target} fuzz/fuzz_${target}.cpp fuzz/replay_main.cpp fuzz/FuzzCommon.h)
    endif()
    target_link_libraries(replay_${target} PRIVATE schneidi_cli_lib)
    add_test(NAME replay_${target} COMMAND replay_${target} ${CMAKE_CURRENT_SOURCE_DIR}/fuzz/corpus/${target}
                                                            ${CMAKE_CURRENT_SOURCE_DIR}/fuzz/regressions/${target})
    set_tests_properties(replay_${target} PROPERTIES
        TIMEOUT 300
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;SCHNEIDI_AUDIO_DEVICE=null;SCHNEIDI_GPU=0")
endforeach()

# Benchmark, not run by CTest
add_executable(bench_filmstrips bench_filmstrips.cpp)
target_link_libraries(bench_filmstrips PRIVATE schneidi_lib)
add_executable(bench_engine bench_engine.cpp check.h)
target_link_libraries(bench_engine PRIVATE schneidi_lib Qt6::Test)
add_executable(bench_playback bench_playback.cpp check.h)
target_link_libraries(bench_playback PRIVATE schneidi_lib Qt6::Test)
add_executable(bench_seek bench_seek.cpp check.h)
target_link_libraries(bench_seek PRIVATE schneidi_lib Qt6::Test)
add_executable(bench_multicam bench_multicam.cpp check.h)
target_link_libraries(bench_multicam PRIVATE schneidi_lib Qt6::Test)
add_executable(bench_probe bench_probe.cpp check.h)
target_link_libraries(bench_probe PRIVATE schneidi_lib Qt6::Test)
add_executable(bench_compose bench_compose.cpp check.h)
target_link_libraries(bench_compose PRIVATE schneidi_lib Qt6::Test)
add_executable(bench_export bench_export.cpp check.h)
target_link_libraries(bench_export PRIVATE schneidi_lib Qt6::Test)
add_executable(bench_undo bench_undo.cpp check.h)
target_link_libraries(bench_undo PRIVATE schneidi_lib Qt6::Test)
add_executable(bench_device_loss bench_device_loss.cpp check.h)
target_link_libraries(bench_device_loss PRIVATE schneidi_lib Qt6::Test)
add_executable(bench_graded_thumbs bench_graded_thumbs.cpp check.h)
target_link_libraries(bench_graded_thumbs PRIVATE schneidi_lib Qt6::Test)
add_executable(bench_memory bench_memory.cpp check.h)
target_link_libraries(bench_memory PRIVATE schneidi_lib Qt6::Test)
if(WIN32) # WASAPI loopback: what the system sends to the sound card
    add_executable(bench_soundcard bench_soundcard.cpp check.h)
    target_link_libraries(bench_soundcard PRIVATE schneidi_lib Qt6::Test)
    target_include_directories(bench_soundcard PRIVATE ${MINIAUDIO_INCLUDE_DIR})
endif()
add_executable(bench_render_start bench_render_start.cpp check.h)
target_link_libraries(bench_render_start PRIVATE schneidi_lib Qt6::Test)
# Color page correction: baked tables against the per-pixel reference (speed and deviation, 1080p)
add_executable(bench_color_grade bench_color_grade.cpp check.h)
target_link_libraries(bench_color_grade PRIVATE schneidi_lib)
# Own time stretcher: quality/speed, against Rubber Band when it is installed (comparison only, the app does not use it)
add_executable(bench_stretch bench_stretch.cpp check.h)
target_link_libraries(bench_stretch PRIVATE schneidi_lib)
pkg_check_modules(BENCH_RUBBERBAND QUIET IMPORTED_TARGET rubberband)
if(BENCH_RUBBERBAND_FOUND)
    target_link_libraries(bench_stretch PRIVATE PkgConfig::BENCH_RUBBERBAND)
    target_compile_definitions(bench_stretch PRIVATE SCHNEIDI_HAVE_RUBBERBAND=1)
endif()
schneidi_test(test_voice_clean) # voice cleaning (RNNoise): sync at any rate, noise drop, stereo, mixer compensation, editor, CLI op
target_link_libraries(test_voice_clean PRIVATE schneidi_cli_lib)
