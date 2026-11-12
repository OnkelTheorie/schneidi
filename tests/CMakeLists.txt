# Automatische Tests (CTest). Bauen/Ausführen:
#   cmake -S . -B build-tests -G Ninja -DSCHNEIDI_TESTS=ON && cmake --build build-tests
#   ctest --test-dir build-tests --output-on-failure
# Alle Tests laufen ohne sichtbare Fenster (offscreen) und ohne Ton (eigenes Gerät "null").

find_package(Qt6 REQUIRED COMPONENTS Widgets Test)

# Tests link against schneidi_lib from the top-level CMakeLists.txt (same sources as the app)

# check.h's out-of-line part (Check::initEngine): keeps the engine headers out of every test. render_check.h = check.h
# plus the frame helpers, for the tests that render.
add_library(schneidi_test_support STATIC check_support.cpp check.h render_check.h)
target_link_libraries(schneidi_test_support PUBLIC schneidi_lib)

# Exit code 77 = skipped (e.g. ffmpeg missing). Every tests/test_*.cpp is registered by itself (glob at the end): a
# new test needs a line here only for extras (schneidi_cli_lib, defines, labels). Calling it again is harmless.
function(schneidi_test name)
    if(TARGET ${name})
        return()
    endif()
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE schneidi_test_support Qt6::Test)
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
schneidi_test(test_interchange_app)
schneidi_test(test_projectfile)
schneidi_test(test_generators)
target_link_libraries(test_generators PRIVATE schneidi_cli_lib)
schneidi_test(test_audio_stream_arg) # CLI analyses: audio_stream not given = the first stream that plays
target_link_libraries(test_audio_stream_arg PRIVATE schneidi_cli_lib)
schneidi_test(test_project_versions)
schneidi_test(test_undo)
schneidi_test(test_h264_encoder) # own H.264 encoder: bit-exact against FFmpeg's decoder (sources in schneidi_lib)
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
schneidi_test(test_voiceover) # voice-over: sound input into a WAV (stand-in input), lining up, track choice, live take, placing, CLI op
target_link_libraries(test_voiceover PRIVATE schneidi_cli_lib)
schneidi_test(test_voiceover_app) # voice-over in the app: arm, record button, stop by playback, take on the track, undo
# plays in real time: under parallel load the preview could fall behind (gaps in the sound = false clicks)
set_tests_properties(test_preview_audio PROPERTIES RUN_SERIAL TRUE)
schneidi_test(test_mediapool)
schneidi_test(test_pool_storage) # Media Pool filter + keys, Media Storage marks, hover scrub, look before import
schneidi_test(test_pool_index) # Media Pool material search: hits from the material index, double-click to the source
schneidi_test(test_pool_remove) # removing clips from the Media Pool: core op, undo, watch, AI turn, CLI op, keys, question
target_link_libraries(test_pool_remove PRIVATE schneidi_cli_lib)
schneidi_test(test_color)
schneidi_test(test_color_bake)
schneidi_test(test_grade_windows)
schneidi_test(test_comp_graph) # compositing graph model (0.19): storage form, validation, order, media paths, node registry
schneidi_test(test_workspace)
schneidi_test(test_luts)
schneidi_test(test_lumas)
schneidi_test(test_key_pick)
schneidi_test(test_viewer_large) # large viewer: panels hide and come back exactly, timeline zoom/scroll untouched
schneidi_test(test_second_screen) # preview on a second monitor: screen choice (fake screens), modes, layout back, unplugged
schneidi_test(test_frame_view) # viewer picture widget: GPU/painter choice, zoom/pan, overlay mapping, zoom actions
schneidi_test(test_picture_gpu) # picture engine on the GPU vs the CPU path (skips without a hardware device)
if(SCHNEIDI_QRHI_HEADER) # QShader for the run-time shader passes
    target_link_libraries(test_picture_gpu PRIVATE Qt6::GuiPrivate)
    target_compile_definitions(test_picture_gpu PRIVATE SCHNEIDI_HAVE_RHI)
endif()
schneidi_test(test_launch_args) # app command line: options (also Qt's X11 -geometry) are no files to import
schneidi_test(test_picture_gpu_render) # Renderer's GPU path vs its CPU path, whole frames (skips without a GPU)
# The GPU decides by itself here (every other test runs the CPU path: SCHNEIDI_GPU=0)
set_tests_properties(test_picture_gpu_render PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;SCHNEIDI_AUDIO_DEVICE=null")
# Preview resolution: the same program once more with the GPU deciding by itself (the frame reaching the viewer on the
# card's path too; says so and checks the CPU path only where there is no usable GPU)
schneidi_test(test_preview_resolution)
add_test(NAME test_preview_resolution_gpu COMMAND test_preview_resolution)
set_tests_properties(test_preview_resolution_gpu PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300
    RESOURCE_LOCK test_preview_resolution ENVIRONMENT "QT_QPA_PLATFORM=offscreen;SCHNEIDI_AUDIO_DEVICE=null")
schneidi_test(test_own_effects)
# Custom effects (0.19): FFmpeg filter chain (pixels, refused filters, errors, comp fx node), shader compile errors,
# project file round trip; through the CLI
schneidi_test(test_custom_effects)
target_link_libraries(test_custom_effects PRIVATE schneidi_cli_lib)
schneidi_test(test_code_trust) # trust for custom code (0.19): untrusted skipped, check/info/render say so, trust_custom_code, records, recipes
target_link_libraries(test_code_trust PRIVATE schneidi_cli_lib)
# The shader on the graphics card (the GPU decides by itself; skips without one or without qsb)
schneidi_test(test_custom_shader)
set_tests_properties(test_custom_shader PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;SCHNEIDI_AUDIO_DEVICE=null")
schneidi_test(test_presets)
schneidi_test(test_look_edit) # bundled look "Edit": listed, one undo step, colours and a mild glow
target_link_libraries(test_look_edit PRIVATE schneidi_cli_lib)
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
# Own H.264 encoder as the export format h264_own: containers, colour tags, level, key frames, bit rate mode, cancel
schneidi_test(test_export_own_h264)
schneidi_test(test_export_timing)
schneidi_test(test_smart_render)
schneidi_test(test_storage_thumbs) # Media Storage thumbnails: key frame near 1 s decoded alone (rotation, VP9, 10-bit, MJPEG lowres), disk cache
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
schneidi_test(test_claude_offer) # when schneidi offers to connect Claude: found, not connected, how often
target_link_libraries(test_claude_offer PRIVATE schneidi_cli_lib)
schneidi_test(test_clip_tilt)
target_link_libraries(test_clip_tilt PRIVATE schneidi_cli_lib)
schneidi_test(test_loudness_target) # 0.19: target_lufs and normalize lufs reach the target after the master limiter
target_link_libraries(test_loudness_target PRIVATE schneidi_cli_lib)
schneidi_test(test_jl_cuts) # J/L cuts (sound edge alone) and room tone in the gaps of a dialogue track
target_link_libraries(test_jl_cuts PRIVATE schneidi_cli_lib)
schneidi_test(test_rough_cut) # 0.17: rough_cut from a script, best_takes, core/ScriptAlign (takes, rating, repeats)
target_link_libraries(test_rough_cut PRIVATE schneidi_cli_lib)
schneidi_test(test_hide_jumps) # 0.17: hide_jumps (alternating zoom at jump cuts), core/JumpCut, check jump_cut
target_link_libraries(test_hide_jumps PRIVATE schneidi_cli_lib)
schneidi_test(test_broll) # 0.17: broll (index shots over what is said, picture only, no shot twice)
target_link_libraries(test_broll PRIVATE schneidi_cli_lib)
schneidi_test(test_story_fields) # 0.17: story sections/notes on a timeline (file, duplicate, retime, undo, info), 'why' on ops
target_link_libraries(test_story_fields PRIVATE schneidi_cli_lib)
schneidi_test(test_story_pace) # 0.17: `check` pace (changes, pace_slow, suggestions), story checks, retention CSV
target_link_libraries(test_story_pace PRIVATE schneidi_cli_lib)
schneidi_test(test_story_ops) # 0.17: section/sections/note/hook_variants ops, core/Story (teaser, suggestion, shift)
target_link_libraries(test_story_ops PRIVATE schneidi_cli_lib)
schneidi_test(test_live)
schneidi_test(test_transcribe_pick) # which audio stream/track transcribe hears (a fake whisper-cli)
target_link_libraries(test_transcribe_pick PRIVATE schneidi_cli_lib)
add_dependencies(test_transcribe_pick schneidi-cli)
schneidi_test(test_transcript_cut)
schneidi_test(test_captions) # word captions: timing, edits, CLI op, render, embedded text streams
target_link_libraries(test_captions PRIVATE schneidi_cli_lib)
schneidi_test(test_short) # `short`: moments, Shorts/Reels/TikTok presets, check platform, limiter, end to end
target_link_libraries(test_short PRIVATE schneidi_cli_lib)
schneidi_test(test_review_render) # `render review`: marks burned in at their frames, sheet tiles, offsets, project untouched
target_link_libraries(test_review_render PRIVATE schneidi_cli_lib)
schneidi_test(test_deliver_review) # Deliver › Prüf-Render: review jobs in the queue with the marks, normal ones without
target_link_libraries(test_deliver_review PRIVATE schneidi_cli_lib)
schneidi_test(test_speech) # text to speech: piper extension, cache, fake piper, CLI op speak
target_link_libraries(test_speech PRIVATE schneidi_cli_lib)
target_link_libraries(test_live PRIVATE schneidi_cli_lib)
add_dependencies(test_live schneidi-cli)
# The 0.16 edit ops live in the app: one undo step each, undo exact, same result as on the file
schneidi_test(test_live_ops016)
target_link_libraries(test_live_ops016 PRIVATE schneidi_cli_lib)
add_dependencies(test_live_ops016 schneidi-cli)
schneidi_test(test_live_talk017) # jl_cut and room_tone live in the app: one undo step, same as on the file
target_link_libraries(test_live_talk017 PRIVATE schneidi_cli_lib)
add_dependencies(test_live_talk017 schneidi-cli)
# 0.17 plans: edit plan:true changes nothing, plan_id + run/discard, stale/expired/unknown, live = file, plan mode
schneidi_test(test_edit_plan)
target_link_libraries(test_edit_plan PRIVATE schneidi_cli_lib)
add_dependencies(test_edit_plan schneidi-cli)
# Files handed to the running app (single instance over the live connection), videos opened as projects, `app open`,
# the real program offscreen
schneidi_test(test_open_in_app)
target_link_libraries(test_open_in_app PRIVATE schneidi_cli_lib)
add_dependencies(test_open_in_app schneidi-cli schneidi)
# The display goes away under the playing app (Xvfb killed): exit code 1, never a crash in the worker threads
schneidi_test(test_display_loss)
add_dependencies(test_display_loss schneidi)
# In-app tours: the live connection waits while one runs (Cli::Live::Server)
schneidi_test(test_tours)
target_link_libraries(test_tours PRIVATE schneidi_cli_lib)
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
# `fit_music` (a song cut to a length on its phrases, its own end, crossfades without clicks) and `snap_to_beat`;
# live in the app one undo step, the same as on the file
schneidi_test(test_fit_music)
# Clip › "Musik auf Länge…": the dialog onto op fit_music (one undo step, errors in the user's language)
schneidi_test(test_fit_music_app)
target_link_libraries(test_fit_music_app PRIVATE schneidi_cli_lib)
target_link_libraries(test_fit_music PRIVATE schneidi_cli_lib)
add_dependencies(test_fit_music schneidi-cli)
schneidi_test(test_path_key) # path keys: absolute, clean, lower case on Windows; caches/backups use them
schneidi_test(test_media_index) # `index`: shots, quality, cache, annotate, search, timeline times
target_link_libraries(test_media_index PRIVATE schneidi_cli_lib)
schneidi_test(test_audio_events) # sound events (heuristics): speech, music, silence, laughter, applause, cheering, shots, screams; cache; peaks {events}
target_link_libraries(test_audio_events PRIVATE schneidi_cli_lib)
schneidi_test(test_audio_events_yamnet) # the real YAMNet model; skipped unless the audio-events extension is installed
# ONNX Runtime by QLibrary with tiny models built in the test, and the YAMNet path with a stand-in model. Needs an
# onnxruntime library here (none = skipped; never downloaded): a system one, or SCHNEIDI_ONNXRUNTIME at run time
schneidi_test(test_inference)
find_library(SCHNEIDI_TEST_ONNXRUNTIME NAMES onnxruntime)
if(NOT SCHNEIDI_TEST_ONNXRUNTIME AND CMAKE_LIBRARY_ARCHITECTURE)
    file(GLOB _ort "/usr/lib/${CMAKE_LIBRARY_ARCHITECTURE}/libonnxruntime.so.*")
    if(_ort)
        list(GET _ort 0 SCHNEIDI_TEST_ONNXRUNTIME)
    endif()
endif()
if(SCHNEIDI_TEST_ONNXRUNTIME)
    target_compile_definitions(test_inference PRIVATE SCHNEIDI_TEST_ONNXRUNTIME="${SCHNEIDI_TEST_ONNXRUNTIME}")
endif()
# YuNet faces (engine/FaceDetect), the faces/onnxruntime catalog entries, faces in `index`: the pure part always, the
# model part with SCHNEIDI_YUNET = face_detection_yunet_2023mar.onnx at run time (else skipped; never downloaded)
schneidi_test(test_face_detect)
target_link_libraries(test_face_detect PRIVATE schneidi_cli_lib)
if(SCHNEIDI_TEST_ONNXRUNTIME)
    target_compile_definitions(test_face_detect PRIVATE SCHNEIDI_TEST_ONNXRUNTIME="${SCHNEIDI_TEST_ONNXRUNTIME}")
endif()
target_link_libraries(test_check PRIVATE schneidi_cli_lib)
target_link_libraries(test_setup PRIVATE schneidi_cli_lib)
add_dependencies(test_setup schneidi-cli)
# MCP over HTTP: real server processes (workers are schneidi-cli mcp) on a free port
schneidi_test(test_serve)
target_link_libraries(test_serve PRIVATE schneidi_cli_lib)
add_dependencies(test_serve schneidi-cli)
schneidi_test(test_audio_regression)
# Reference renders of the sound (own audio engine, docs/archiv/plan-0.10-ton.md): references in the private docs folder
schneidi_test(test_audio_reference)
schneidi_test(test_av_sync)
schneidi_test(test_audio_reader)
schneidi_test(test_audio_reader_damage) # vanishing files, timestamp gaps and outliers, Matroska without duration
schneidi_test(test_media_probe)
schneidi_test(test_media_unreadable) # pipes and devices, undecodable stills, sub-frame sounds, why a file is unreadable
schneidi_test(test_audio_mixer)
schneidi_test(test_stretch) # own time stretcher (keep pitch)
schneidi_test(test_export_audio)
schneidi_test(test_audio_analysis)
schneidi_test(test_audio_player)
set_tests_properties(test_audio_player PROPERTIES RUN_SERIAL TRUE) # real time
schneidi_test(test_play_end) # playback stops at the end and stays stopped
schneidi_test(test_play_loop) # loop playback in the In/Out range: wraps Out -> In, no click, off/removed plays to the end
set_tests_properties(test_av_sync PROPERTIES RUN_SERIAL TRUE) # preview timing
schneidi_test(test_dynamics_eq)
target_link_libraries(test_dynamics_eq PRIVATE schneidi_cli_lib)
schneidi_test(test_audio_sync)
schneidi_test(test_multicam)
schneidi_test(test_multicam_render)
schneidi_test(test_multicam_viewer)
schneidi_test(test_proxy_hint)
schneidi_test(test_autosave_restore) # restore after a crash: a broken backup says why and is moved aside
schneidi_test(test_media_timing) # proxy frame k = original frame k, shorter sources keep later clips in place, frame positions
schneidi_test(test_proxy_cleanup) # which proxy files cleaning up deletes, which stay, dry run
schneidi_test(test_proxy_quality) # proxy size per quality level, changing it, cleanup, the viewer note
schneidi_test(test_memory_release) # picture pools and the heap given back while idle, never during playback
schneidi_test(test_settings) # app settings (File › Settings…), AI page, indicator modes
schneidi_test(test_update_check) # update hint against a local HTTP server
schneidi_test(test_ai_activity) # AI changes light up, AI log, indicator with the AI's work
schneidi_test(test_ai_providers) # AI chat: requests/answers of both protocols, keys, company file (no network)
# AI chat end to end: MainWindow + live server, the real schneidi-cli mcp, a fake provider on 127.0.0.1
schneidi_test(test_ai_chat)
target_link_libraries(test_ai_chat PRIVATE schneidi_cli_lib)
add_dependencies(test_ai_chat schneidi-cli)
schneidi_test(test_multicam_cli)
target_link_libraries(test_multicam_cli PRIVATE schneidi_cli_lib)
schneidi_test(test_stream_presets)
target_link_libraries(test_stream_presets PRIVATE schneidi_cli_lib)
schneidi_test(test_linked_ops) # delete/freeze on linked clips act on the named ones
target_link_libraries(test_linked_ops PRIVATE schneidi_cli_lib)
schneidi_test(test_template_fill) # content templates: placeholders in the project file, filling them, template folders
schneidi_test(test_template_app) # content templates in the app: placeholders (menu, Inspector, drag-fill), browser, save, check fields
schneidi_test(test_start_templates) # how new projects look: start templates (app, settings page, CLI new/templates)
target_link_libraries(test_start_templates PRIVATE schneidi_cli_lib)
schneidi_test(test_brand_kit) # brand kit: JSON, global file, project file, applyBrand, logo/intro, CLI brand
target_link_libraries(test_brand_kit PRIVATE schneidi_cli_lib)
schneidi_test(test_template_cli) # `templates`: list, info, fill, edit ops slot/fill, ZIP export/import, live fill
target_link_libraries(test_template_cli PRIVATE schneidi_cli_lib)
add_dependencies(test_template_cli schneidi-cli)
schneidi_test(test_data_rows) # tables for many videos from a template: CSV/JSON rows, file names from a pattern
schneidi_test(test_template_batch) # `templates` fill + rows: one project (and video) per row, bad rows, dry_run
target_link_libraries(test_template_batch PRIVATE schneidi_cli_lib)
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
add_dependencies(test_watch_folders schneidi-cli)
schneidi_test(test_speed)
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
             test_ducking test_crop test_blur_fill test_reframe test_reframe_keep test_game43 test_generators test_fit_music
             test_story_pace test_review_render test_data_rows)
    add_test(NAME ${name}_de_DE COMMAND ${name})
    set_tests_properties(${name}_de_DE PROPERTIES
        SKIP_RETURN_CODE 77
        TIMEOUT 300
        RESOURCE_LOCK ${name}
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;SCHNEIDI_AUDIO_DEVICE=null;SCHNEIDI_GPU=0;LC_ALL=de_DE.UTF-8;LANG=de_DE.UTF-8")
endforeach()

set_tests_properties(test_av_sync test_preview_audio test_play_end test_play_loop PROPERTIES RUN_SERIAL TRUE) # preview timing

# Label "timing": tests that measure real time or wait for other processes. They fail when the machine is busy (other
# builds, a game, a parallel ctest), not when the code is wrong. tools/run_tests.py runs everything else in parallel,
# then these one by one, and once more what failed (ctest -LE timing / -L timing work by hand too).
set_tests_properties(test_av_sync test_preview_audio test_play_end test_play_loop test_audio_player test_speed test_serve
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
add_executable(bench_storage_thumbs bench_storage_thumbs.cpp)
target_link_libraries(bench_storage_thumbs PRIVATE schneidi_lib)
add_executable(bench_engine bench_engine.cpp)
target_link_libraries(bench_engine PRIVATE schneidi_test_support Qt6::Test)
add_executable(bench_playback bench_playback.cpp)
target_link_libraries(bench_playback PRIVATE schneidi_test_support Qt6::Test)
add_executable(bench_transition bench_transition.cpp)
target_link_libraries(bench_transition PRIVATE schneidi_test_support Qt6::Test)
add_executable(bench_seek bench_seek.cpp)
target_link_libraries(bench_seek PRIVATE schneidi_test_support Qt6::Test)
add_executable(bench_multicam bench_multicam.cpp)
target_link_libraries(bench_multicam PRIVATE schneidi_test_support Qt6::Test)
add_executable(bench_probe bench_probe.cpp)
target_link_libraries(bench_probe PRIVATE schneidi_test_support Qt6::Test)
add_executable(bench_compose bench_compose.cpp)
target_link_libraries(bench_compose PRIVATE schneidi_test_support Qt6::Test)
add_executable(bench_export bench_export.cpp)
target_link_libraries(bench_export PRIVATE schneidi_test_support Qt6::Test)
add_executable(bench_undo bench_undo.cpp)
target_link_libraries(bench_undo PRIVATE schneidi_test_support Qt6::Test)
add_executable(bench_device_loss bench_device_loss.cpp)
target_link_libraries(bench_device_loss PRIVATE schneidi_test_support Qt6::Test)
add_executable(bench_graded_thumbs bench_graded_thumbs.cpp)
target_link_libraries(bench_graded_thumbs PRIVATE schneidi_test_support Qt6::Test)
add_executable(bench_memory bench_memory.cpp)
target_link_libraries(bench_memory PRIVATE schneidi_test_support Qt6::Test)
add_executable(bench_play_after_pause bench_play_after_pause.cpp)
target_link_libraries(bench_play_after_pause PRIVATE schneidi_test_support Qt6::Test)
if(WIN32) # WASAPI loopback: what the system sends to the sound card
    add_executable(bench_soundcard bench_soundcard.cpp)
    target_link_libraries(bench_soundcard PRIVATE schneidi_test_support Qt6::Test)
    target_include_directories(bench_soundcard SYSTEM PRIVATE ${MINIAUDIO_INCLUDE_DIR})
endif()
add_executable(bench_render_start bench_render_start.cpp)
target_link_libraries(bench_render_start PRIVATE schneidi_test_support Qt6::Test)
# Color page correction: baked tables against the per-pixel reference (speed and deviation, 1080p)
add_executable(bench_color_grade bench_color_grade.cpp)
target_link_libraries(bench_color_grade PRIVATE schneidi_test_support)
# Own time stretcher: quality/speed, against Rubber Band when it is installed (comparison only, the app does not use it)
add_executable(bench_stretch bench_stretch.cpp)
target_link_libraries(bench_stretch PRIVATE schneidi_test_support)
pkg_check_modules(BENCH_RUBBERBAND QUIET IMPORTED_TARGET rubberband)
if(BENCH_RUBBERBAND_FOUND)
    target_link_libraries(bench_stretch PRIVATE PkgConfig::BENCH_RUBBERBAND)
    target_compile_definitions(bench_stretch PRIVATE SCHNEIDI_HAVE_RUBBERBAND=1)
endif()
schneidi_test(test_voice_clean) # voice cleaning (RNNoise): sync at any rate, noise drop, stereo, mixer compensation, editor, CLI op
target_link_libraries(test_voice_clean PRIVATE schneidi_cli_lib)
schneidi_test(test_sfx) # effect sounds: synthesis deterministic and mild, placement rules, op sfx
target_link_libraries(test_sfx PRIVATE schneidi_cli_lib)
schneidi_test(test_sfx_library) # Effects Library "Klänge": list, drag data, drop = op sfx as one undo step, nothing placed by itself
target_link_libraries(test_sfx_library PRIVATE schneidi_cli_lib)
schneidi_test(test_builtin_templates) # the five built-in templates: listed, filled, brand kit, logo default, size guard
target_link_libraries(test_builtin_templates PRIVATE schneidi_cli_lib)

# RNNoise's portable C kernels (the path every non-x86 build takes: ARM Linux/Raspberry Pi, Windows/macOS on ARM) -
# compiled here on x86 too, so a header missing from the vendored sources shows up before an ARM build breaks
foreach(_rnn_src nnet.c nnet_default.c)
    add_test(NAME test_rnnoise_generic_c_${_rnn_src}
        COMMAND ${CMAKE_C_COMPILER} -fsyntax-only -w -U__SSE2__ -U__AVX__ -DRNNOISE_BUILD
            -I${PROJECT_SOURCE_DIR}/third_party/rnnoise/src -I${PROJECT_SOURCE_DIR}/third_party/rnnoise/include
            ${PROJECT_SOURCE_DIR}/third_party/rnnoise/src/${_rnn_src})
endforeach()
schneidi_test(test_title_style) # title looks: gradient/texture fill, 2.5D edge, shadow, glow, file, op title, nodes text/shadow/glitch
target_link_libraries(test_title_style PRIVATE schneidi_cli_lib)
schneidi_test(test_comp_masks) # mask nodes (0.19): shapes, soft edge, invert, combine, follow, matte, rect/ellipse == Color page window, timing
schneidi_test(test_comp_key) # keyer nodes (0.19): chroma = effect chromakey, luma thresholds, matte_tune grow/shrink/clip/soft/despill
schneidi_test(test_comp_tilt) # tilting in depth (0.20): homography, tilted placement, projective draw = affine draw, transform node tilt
# Tracker node (0.19, package D): one point, two points (size, rotation), occlusion, faces by a stand-in, keyframes + undo
schneidi_test(test_comp_tracker)
# The same in face mode with the real YuNet model; 77 without the extension
schneidi_test(test_comp_tracker_face)
schneidi_test(test_comp_eval) # compositing evaluation (0.19 A): merge modes vs formula, graph = effect list bit-identical, comp_from, cache, unknown kinds
schneidi_test(test_comp_follow) # tracker follow rule (0.19): followers relative to rx/ry, no jump when the analysis lands, transform/mask alike
schneidi_test(test_comp_timeline) # comp graphs through timeline edits: media offset and text from/until follow cuts and head trims, slip keeps them, undo
schneidi_test(test_comp_cli) # compositing through the CLI (0.19 G): ops comp/comp_from/node/track_node, keyframes comp.n<id>.<key>, effects kind nodes, frames at a node, comp checks, undo
target_link_libraries(test_comp_cli PRIVATE schneidi_cli_lib)
schneidi_test(test_comp_tracker_lost) # losing the tracked point (0.22): seen keys, lost modes bridge/hold/fade of masks and transforms, check tracker_lost/_ended, CLI
target_link_libraries(test_comp_tracker_lost PRIVATE schneidi_cli_lib)
schneidi_test(test_comp_corner_pin) # four-corner tracking (0.22): QuadTracker, mode quad, node corner_pin, CLI track_node corners, recipe screen_replace
target_link_libraries(test_comp_corner_pin PRIVATE schneidi_cli_lib)
# Compositing graphs on the graphics card (0.19 package F) vs CompEval: merge modes, masks, keys, blur, recipes;
# decides about the card itself (77 without a hardware device)
schneidi_test(test_comp_gpu)
set_tests_properties(test_comp_gpu PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;SCHNEIDI_AUDIO_DEVICE=null")
schneidi_test(test_comp_recipes) # compositing recipes (0.19 I): built-in ones apply/validate/render, slots, errors, save/list/apply, ZIP, undo
target_link_libraries(test_comp_recipes PRIVATE schneidi_cli_lib)
schneidi_test(test_comp_page) # Comp page: lazy page, layer view, canvas add/link/remove/duplicate, handles, keyframe diamond, undo

# AI cutout (0.19): pre/post-processing, matte cache, smoothing, guided upsample, node, analysis + CLI op with a
# stand-in model, Editor op undo, missing extension
schneidi_test(test_comp_cutout)
target_link_libraries(test_comp_cutout PRIVATE schneidi_cli_lib)
# The same with the real segmentation model; 77 without the extension
schneidi_test(test_comp_cutout_model)

schneidi_test(test_text3d) # real 3D text (0.19): "O" hole, cap area = outline area, side at 90°, light, AA, cache, project, plain title bit-identical

schneidi_test(test_trust_dialog) # custom code from someone else: the open dialog (run/skip), remembered, inspector "Code erlauben",
# trust_custom_code of an AI in the app asks the user
target_link_libraries(test_trust_dialog PRIVATE schneidi_cli_lib)
schneidi_test(test_effect_aliases) # readable names of index-keyed effect parameters (0.19): op effect, keyframe, fx node, info, effects
target_link_libraries(test_effect_aliases PRIVATE schneidi_cli_lib)
schneidi_test(test_title_anim) # title entry/exit animation (0.19): pop/fade/slide sizes and opacity, still frames bit-identical, file, op title
target_link_libraries(test_title_anim PRIVATE schneidi_cli_lib)
schneidi_test(test_subtitles_readable) # readable subtitle cues (0.19): lines, cps, pauses, word times, check, op subtitles readable/words/track
target_link_libraries(test_subtitles_readable PRIVATE schneidi_cli_lib)
schneidi_test(test_settings_isolation) # SCHNEIDI_TEST_SETTINGS: a spawned schneidi-cli reads the test's settings, never the user's

# Every test_*.cpp without a schneidi_test() line above (CONFIGURE_DEPENDS: a new file is picked up by the next build)
file(GLOB _schneidi_test_sources CONFIGURE_DEPENDS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp)
foreach(_src ${_schneidi_test_sources})
    get_filename_component(_name ${_src} NAME_WE)
    schneidi_test(${_name})
endforeach()

# Paths the tests need at run time (Check::buildPath in check.h). No compile definitions: an absolute path there differs
# per worktree, so ccache could never share those objects. ctest passes them in the environment (all tests of this
# folder, added to what each test sets itself); a test started by hand reads schneidi-test-paths.ini next to itself.
set(SCHNEIDI_TEST_PATHS
    "SCHNEIDI_CLI=$<TARGET_FILE:schneidi-cli>"
    "SCHNEIDI_APP=$<TARGET_FILE:schneidi>"
    "SCHNEIDI_TEST_DATA=${CMAKE_CURRENT_SOURCE_DIR}/data"
    "SCHNEIDI_AUDIO_REFERENCE=${CMAKE_SOURCE_DIR}/docs/testdaten/ton-referenz"
    "SCHNEIDI_SOURCE_DIR=${CMAKE_SOURCE_DIR}")
list(JOIN SCHNEIDI_TEST_PATHS "\n" _schneidi_test_paths_ini)
file(GENERATE OUTPUT "$<TARGET_FILE_DIR:schneidi_test_support>/schneidi-test-paths.ini"
     CONTENT "${_schneidi_test_paths_ini}\n")
list(TRANSFORM SCHNEIDI_TEST_PATHS REPLACE "^([A-Z_]+)=" "\\1=set:" OUTPUT_VARIABLE _schneidi_test_env)
get_property(_schneidi_tests DIRECTORY PROPERTY TESTS)
set_property(TEST ${_schneidi_tests} APPEND PROPERTY ENVIRONMENT_MODIFICATION ${_schneidi_test_env})
add_dependencies(test_settings_isolation schneidi-cli)

# Never the user's settings, data, config or cache (core/SettingsIsolation.h, Check::isolateSettings): every test gets
# its own folder, honoured by the test and by every schneidi/schneidi-cli it starts (also on Windows, where XDG does
# nothing). Last in this file: covers every test registered above, variants included.
get_property(_schneidi_all_tests DIRECTORY PROPERTY TESTS)
foreach(_t ${_schneidi_all_tests})
    set(_dir "${CMAKE_CURRENT_BINARY_DIR}/test-settings/${_t}")
    set_property(TEST ${_t} APPEND PROPERTY ENVIRONMENT_MODIFICATION
        "SCHNEIDI_TEST_SETTINGS=set:${_dir}" "XDG_CONFIG_HOME=set:${_dir}/config" "XDG_DATA_HOME=set:${_dir}/data"
        "XDG_CACHE_HOME=set:${_dir}/cache" "SCHNEIDI_TEST_SETTINGS_READY=unset:")
endforeach()
