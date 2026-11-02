# Writes buildinfo.h with the Git state (e.g. "06d56c5", with "+" for uncommitted changes), so every report can be
# matched to a state. Runs on every build (cmake -P), writes only on change.
# Inputs: SRC (project folder), OUT (target file), GIT (git program, may be missing), VERSION (released version),
# NEXT (version being worked on, may be empty), BUILD_ID (given by a packaging script, may be empty; also the
# environment variable SCHNEIDI_BUILD_ID)
# Without a Git checkout (a container or the Windows VM building from `git archive`): the commit git archive wrote
# into cmake/archive-commit.txt (export-subst in .gitattributes); nothing known = no build id (only the version shows)
set(build "")
if(NOT BUILD_ID)
    set(BUILD_ID "$ENV{SCHNEIDI_BUILD_ID}")
endif()
if(BUILD_ID)
    set(build "${BUILD_ID}")
elseif(GIT)
    execute_process(COMMAND "${GIT}" -C "${SRC}" rev-parse --short=7 HEAD
                    OUTPUT_VARIABLE hash OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE rc)
    if(rc EQUAL 0 AND hash)
        set(build "${hash}")
        execute_process(COMMAND "${GIT}" -C "${SRC}" status --porcelain --untracked-files=no -- src
                        OUTPUT_VARIABLE dirty OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        if(dirty)
            string(APPEND build "+")
        endif()
    endif()
endif()
if(NOT build AND EXISTS "${SRC}/cmake/archive-commit.txt")
    file(STRINGS "${SRC}/cmake/archive-commit.txt" archived LIMIT_COUNT 1)
    if(archived MATCHES "^[0-9a-f]+$")
        string(SUBSTRING "${archived}" 0 7 build)
    endif()
endif()
# Shown version: "0.9.0 → 0.10.0" while working on the next one, so a build past 0.9.0 does not look like 0.9.0
set(label "${VERSION}")
if(NEXT AND NOT NEXT STREQUAL VERSION)
    set(label "${VERSION} → ${NEXT}")
endif()
set(content "#pragma once\n#define SCHNEIDI_VERSION \"${VERSION}\"\n#define SCHNEIDI_VERSION_LABEL \"${label}\"\n#define SCHNEIDI_BUILD \"${build}\"\n")
if(EXISTS "${OUT}")
    file(READ "${OUT}" old)
endif()
if(NOT old STREQUAL content)
    file(WRITE "${OUT}" "${content}")
endif()
