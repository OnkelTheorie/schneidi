# RNNoise (BSD-3-Clause, Xiph.Org / Mozilla / Amazon / Jean-Marc Valin): neural noise suppression for the voice
# cleaning effect (engine/VoiceClean). The sources are vendored in third_party/rnnoise (v0.2, as the mici project uses
# them); the model weights (a ~29 MB generated C file) are not: they come from the release tarball of the model version
# in third_party/rnnoise/model_version, checked against a fixed SHA-256.
# Offline builds: put the tarball next to model_version (third_party/rnnoise/rnnoise_data-<version>.tar.gz, ignored by
# git) or give its path with -DSCHNEIDI_RNNOISE_MODEL=<file>; otherwise it is downloaded once into that folder.
enable_language(C)

set(RNN_DIR ${PROJECT_SOURCE_DIR}/third_party/rnnoise)
file(READ ${RNN_DIR}/model_version RNN_MODEL_VERSION)
string(STRIP "${RNN_MODEL_VERSION}" RNN_MODEL_VERSION)
set(RNN_MODEL_SHA256 4ac81c5c0884ec4bd5907026aaae16209b7b76cd9d7f71af582094a2f98f4b43) # rnnoise_data-0b50c45.tar.gz
set(SCHNEIDI_RNNOISE_MODEL "${RNN_DIR}/rnnoise_data-${RNN_MODEL_VERSION}.tar.gz"
    CACHE FILEPATH "RNNoise model tarball (downloaded when missing)")

set(_rnn_data_c ${CMAKE_BINARY_DIR}/rnnoise_model/src/rnnoise_data.c)
if(NOT EXISTS ${_rnn_data_c})
    if(EXISTS ${SCHNEIDI_RNNOISE_MODEL})
        file(SHA256 ${SCHNEIDI_RNNOISE_MODEL} _rnn_hash)
    endif()
    if(NOT _rnn_hash STREQUAL RNN_MODEL_SHA256)
        message(STATUS "Downloading the RNNoise model ${RNN_MODEL_VERSION} (22 MB) ...")
        file(DOWNLOAD https://media.xiph.org/rnnoise/models/rnnoise_data-${RNN_MODEL_VERSION}.tar.gz
             ${SCHNEIDI_RNNOISE_MODEL} EXPECTED_HASH SHA256=${RNN_MODEL_SHA256} STATUS _rnn_status)
        list(GET _rnn_status 0 _rnn_code)
        if(NOT _rnn_code EQUAL 0)
            file(REMOVE ${SCHNEIDI_RNNOISE_MODEL})
            message(FATAL_ERROR "Could not download the RNNoise model (${_rnn_status}). Offline: copy "
                                "rnnoise_data-${RNN_MODEL_VERSION}.tar.gz from media.xiph.org/rnnoise/models to "
                                "${RNN_DIR}/ or give -DSCHNEIDI_RNNOISE_MODEL=<file>")
        endif()
    endif()
    file(ARCHIVE_EXTRACT INPUT ${SCHNEIDI_RNNOISE_MODEL} DESTINATION ${CMAKE_BINARY_DIR}/rnnoise_model
         PATTERNS src/rnnoise_data.c)
endif()

add_library(rnnoise STATIC
    ${RNN_DIR}/src/denoise.c
    ${RNN_DIR}/src/rnn.c
    ${RNN_DIR}/src/pitch.c
    ${RNN_DIR}/src/kiss_fft.c
    ${RNN_DIR}/src/celt_lpc.c
    ${RNN_DIR}/src/nnet.c
    ${RNN_DIR}/src/nnet_default.c
    ${RNN_DIR}/src/parse_lpcnet_weights.c
    ${RNN_DIR}/src/rnnoise_tables.c
    ${_rnn_data_c})
target_include_directories(rnnoise PUBLIC ${RNN_DIR}/include PRIVATE ${RNN_DIR}/src)
target_compile_definitions(rnnoise PRIVATE RNNOISE_BUILD)
# Third-party code: its warnings are not ours. Always optimized (the network runs per 10 ms of sound)
target_compile_options(rnnoise PRIVATE -w -O2)
set_target_properties(rnnoise PROPERTIES POSITION_INDEPENDENT_CODE ON)
find_library(RNN_LIBM m)
if(RNN_LIBM)
    target_link_libraries(rnnoise PRIVATE ${RNN_LIBM})
endif()
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
    # SSE4.1/AVX2 kernels, chosen at run time by the CPU (the plain C ones otherwise)
    target_sources(rnnoise PRIVATE
        ${RNN_DIR}/src/x86/x86_dnn_map.c
        ${RNN_DIR}/src/x86/x86cpu.c
        ${RNN_DIR}/src/x86/nnet_sse4_1.c
        ${RNN_DIR}/src/x86/nnet_avx2.c)
    target_compile_definitions(rnnoise PRIVATE RNN_ENABLE_X86_RTCD CPU_INFO_BY_ASM)
    set_source_files_properties(${RNN_DIR}/src/x86/nnet_sse4_1.c PROPERTIES COMPILE_OPTIONS "-msse4.1")
    if(MINGW)
        # MinGW GCC does not align the stack to 32 bytes (GCC PR 54412): a spilled __m256 with an aligned move would
        # crash, so the assembler turns aligned vector moves into unaligned ones (same speed on AVX2 CPUs)
        set_source_files_properties(${RNN_DIR}/src/x86/nnet_avx2.c PROPERTIES COMPILE_OPTIONS
            "-mavx2;-mfma;-Wa,-muse-unaligned-vector-move")
    else()
        set_source_files_properties(${RNN_DIR}/src/x86/nnet_avx2.c PROPERTIES COMPILE_OPTIONS "-mavx2;-mfma")
    endif()
endif()
