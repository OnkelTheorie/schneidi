# Schreibt buildinfo.h mit dem Git-Stand (z. B. "06d56c5", mit "+" bei ungespeicherten Änderungen), damit jede
# Rückmeldung einem Stand zugeordnet werden kann. Läuft bei jedem Build (cmake -P), schreibt nur bei Änderung.
# Eingaben: SRC (Projektordner), OUT (Zieldatei), GIT (git-Programm, darf fehlen), VERSION (project-Version)
set(build "unbekannt")
if(GIT)
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
set(content "#pragma once\n#define SCHNEIDI_VERSION \"${VERSION}\"\n#define SCHNEIDI_BUILD \"${build}\"\n")
if(EXISTS "${OUT}")
    file(READ "${OUT}" old)
endif()
if(NOT old STREQUAL content)
    file(WRITE "${OUT}" "${content}")
endif()
