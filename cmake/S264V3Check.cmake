# Checks the x86-64-v3 build of the s264 encoder (one relocatable object, see SCHNEIDI_S264_V3 in CMakeLists.txt)
# before anything links it; run as a step of its custom command and as the test s264_v3_object.
#   cmake -DOBJ=<s264_v3.o> -DREADELF=<readelf> -DNM=<nm> [-DREMOVE_ON_FAIL=ON] -P S264V3Check.cmake
# 1. A real ELF relocatable object, not compiler IR: with LTO (-flto, a distribution's default) the object holds
#    GCC's .gnu.lto_* sections or is LLVM bitcode, the code is generated later at the final link - with the flags of
#    that link, not -march=x86-64-v3, and objcopy could not make its symbols local.
# 2. No code that runs while the program loads or exits (.init_array, .ctors, .preinit_array, .fini_array, .dtors):
#    on a CPU without AVX the v3 code must never run, and nothing but the CPU check decides that. Function-local
#    statics (__cxa_guard_*, __cxa_atexit at their first use) are fine: they run inside the v3 build's own calls.
# 3. Exactly the two entry points global, everything else local (two builds of the same templates and inline
#    functions must never be merged by the linker).
foreach(var OBJ READELF NM)
    if(NOT ${var})
        message(FATAL_ERROR "S264V3Check: ${var} not set")
    endif()
endforeach()

set(_problems "")
execute_process(COMMAND ${READELF} -hSW ${OBJ} OUTPUT_VARIABLE _elf ERROR_VARIABLE _elf_err RESULT_VARIABLE _rc)
if(NOT _rc EQUAL 0)
    list(APPEND _problems "not an ELF object (LLVM bitcode from -flto?): ${_elf_err}")
else()
    if(NOT _elf MATCHES "Type:[ \t]+REL ")
        list(APPEND _problems "not a relocatable object")
    endif()
    foreach(sec .init_array .ctors .preinit_array .fini_array .dtors)
        string(REPLACE "." "\\." _re "${sec}")
        if(_elf MATCHES "\\] ${_re}[. \t]")
            list(APPEND _problems "section ${sec}: code that runs at load or exit (a namespace-scope variable with a dynamic initialiser or destructor - use S264_DEV, constexpr or a function's static, DevEnv.h)")
        endif()
    endforeach()
    if(_elf MATCHES "\\.gnu\\.lto_" OR _elf MATCHES "\\.llvmbc" OR _elf MATCHES "\\.llvm\\.lto")
        list(APPEND _problems "LTO intermediate code in the object (-fno-lto missing?)")
    endif()
    execute_process(COMMAND ${NM} -g --defined-only ${OBJ} OUTPUT_VARIABLE _syms RESULT_VARIABLE _rc)
    string(REGEX MATCHALL "[^\n]+" _lines "${_syms}")
    set(_names "")
    foreach(l IN LISTS _lines)
        string(REGEX REPLACE "^.* " "" _n "${l}")
        list(APPEND _names "${_n}")
    endforeach()
    list(SORT _names)
    if(NOT _names STREQUAL "schneidi_s264_v3_encoder;schneidi_s264_v3_first_pass")
        list(LENGTH _names _count)
        list(SUBLIST _names 0 8 _first)
        list(JOIN _first " " _first)
        list(APPEND _problems "defined global symbols must be exactly the two entry points, are ${_count}: ${_first} ...")
    endif()
endif()

if(_problems)
    if(REMOVE_ON_FAIL)
        file(REMOVE ${OBJ})
    endif()
    list(JOIN _problems "\n  " _text)
    message(FATAL_ERROR "s264: the x86-64-v3 object ${OBJ} is unusable:\n  ${_text}")
endif()
message(STATUS "s264: x86-64-v3 object checked (no load-time code, no LTO, two global symbols)")
