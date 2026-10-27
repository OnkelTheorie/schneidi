# libFuzzer targets for the parts that read input from others (SCHNEIDI_FUZZ=ON with clang, see the top-level
# CMakeLists.txt): one program per kind of input, seeds in corpus/<target>, dictionaries in dict/.
#   fuzz_http     requests to `schneidi-cli serve` (head, body, pipelining, /files paths, Range, Host)
#   fuzz_mcp      MCP / JSON-RPC messages and the argument checks of tools/call (paths, times)
#   fuzz_project  project files (.schneidi): reading, migrations, saving again
#   fuzz_edl, fuzz_fcp7, fuzz_otio   timelines of other programs, imported into a project
#   fuzz_srt      subtitle files
# Run one (from the build folder; the first folder collects new inputs, the others are seeds). All paths absolute:
# fuzz_http, fuzz_mcp and fuzz_project change into their work folder (like the server) before reading them.
#   mkdir -p fuzz-corpus/http fuzz-findings/http && tests/fuzz/fuzz_http -dict=$PWD/../tests/fuzz/dict/http.dict \
#       -max_len=8192 -rss_limit_mb=2048 -timeout=10 -max_total_time=1800 -artifact_prefix=$PWD/fuzz-findings/http/ \
#       $PWD/fuzz-corpus/http $PWD/../tests/fuzz/corpus/http
# A finding is saved as crash-*/timeout-*/oom-* in the -artifact_prefix folder; replay it by passing the file instead
# of folders. Shrink it with -minimize_crash=1 -max_total_time=90 -exact_artifact_path=<out> <file> (did not work for
# fuzz_project, whose inputs are shrunk by hand).

add_custom_target(fuzz_all)

# Links only what the target reads with: schneidi_lib (core) or schneidi_cli_lib (server, MCP, work folder)
function(schneidi_fuzzer name source lib)
    add_executable(${name} ${source} FuzzCommon.h)
    target_link_libraries(${name} PRIVATE ${lib})
    target_link_options(${name} PRIVATE -fsanitize=fuzzer)
    target_compile_definitions(${name} PRIVATE SCHNEIDI_LIBFUZZER) # FuzzJson.h: own mutator for JSON inputs
    add_dependencies(fuzz_all ${name})
endfunction()

schneidi_fuzzer(fuzz_http fuzz_http.cpp schneidi_cli_lib)
schneidi_fuzzer(fuzz_mcp fuzz_mcp.cpp schneidi_cli_lib)
schneidi_fuzzer(fuzz_project fuzz_project.cpp schneidi_cli_lib)
schneidi_fuzzer(fuzz_srt fuzz_srt.cpp schneidi_lib)
foreach(format Edl Fcp7 Otio)
    string(TOLOWER ${format} lower)
    schneidi_fuzzer(fuzz_${lower} fuzz_interchange.cpp schneidi_lib)
    target_compile_definitions(fuzz_${lower} PRIVATE FUZZ_FORMAT=${format})
endforeach()
target_compile_definitions(fuzz_otio PRIVATE FUZZ_JSON_INPUT)
