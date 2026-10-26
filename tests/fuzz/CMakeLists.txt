# libFuzzer targets for the parts that read input from others (SCHNEIDI_FUZZ=ON with clang, see the top-level
# CMakeLists.txt): one program per kind of input, seeds in corpus/<target>, dictionaries in dict/.
#   fuzz_http     requests to `schneidi-cli serve` (head, body, pipelining, /files paths, Range, Host)
#   fuzz_mcp      MCP / JSON-RPC messages and the argument checks of tools/call (paths, times)
#   fuzz_project  project files (.schneidi): reading, migrations, saving again
#   fuzz_edl, fuzz_fcp7, fuzz_otio   timelines of other programs, imported into a project
#   fuzz_srt      subtitle files
# Run one (from the build folder; the first folder collects new inputs, the others are seeds):
#   mkdir -p fuzz-corpus/http && tests/fuzz/fuzz_http -dict=../tests/fuzz/dict/http.dict -max_len=8192 \
#       -rss_limit_mb=2048 -timeout=10 fuzz-corpus/http ../tests/fuzz/corpus/http
# A finding is saved as crash-*/timeout-*/oom-*; shrink it with -minimize_crash=1 -runs=10000 <file>, and replay it
# by passing the file instead of folders.

add_custom_target(fuzz_all)

# Links only what the target reads with: schneidi_lib (core) or schneidi_cli_lib (server, MCP, work folder)
function(schneidi_fuzzer name source lib)
    add_executable(${name} ${source} FuzzCommon.h)
    target_link_libraries(${name} PRIVATE ${lib})
    target_link_options(${name} PRIVATE -fsanitize=fuzzer)
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
