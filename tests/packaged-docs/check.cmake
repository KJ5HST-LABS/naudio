# SPDX-License-Identifier: LGPL-2.1-or-later
#
# naudio — the packaged-documentation gate. Driven by the `naudio_packaged_docs` ctest arm; see
# the add_test() comment in the root CMakeLists.txt for why it exists.
#
# Run as: cmake -D NA_SOURCE_DIR=... -D NA_BUILD_DIR=... -D NA_WORK_DIR=...
#              -D NA_DATAROOTDIR=... -P check.cmake
#
# WHY THIS IS AN INSTALL, NOT A GREP. On 2026-08-31 an audit measured the licence text, the wire
# spec and the conformance vectors ABSENT from all six shipped artifact forms. Every one of them
# was present in the repository the whole time, and CPACK_RESOURCE_FILE_LICENSE was set and looked
# like it covered the licence — it is honoured by NSIS and productbuild and ignored by the archive,
# DEB and RPM generators. So no check that reads the SOURCE TREE or the CMake configuration could
# have caught it. What a package contains is a property of the artifact, and the only honest way to
# assert it is to perform the install and look at what came out.
#
# TWO ARMS, and the second is not decoration. This gate passes by finding everything it expects,
# but a checker whose EXISTS predicate is broken — a mis-joined path, an empty prefix variable —
# would report "all present" against an empty directory just as a correct install does. So arm 2
# runs the same predicate against a path that is deliberately NOT installed and requires it to be
# reported MISSING. Only then does arm 1's success mean the files are really there.

# NA_CONFIG is deliberately NOT in the required list: single-config generators may resolve
# $<CONFIG> to an empty string, and passing an empty --config is worse than passing none.
foreach(var NA_SOURCE_DIR NA_BUILD_DIR NA_WORK_DIR NA_DATAROOTDIR)
    if(NOT DEFINED ${var} OR "${${var}}" STREQUAL "")
        message(FATAL_ERROR "packaged-docs: ${var} was not passed to the script")
    endif()
endforeach()

set(prefix "${NA_WORK_DIR}/prefix")
file(REMOVE_RECURSE "${prefix}")

set(install_cmd "${CMAKE_COMMAND}" --install "${NA_BUILD_DIR}" --prefix "${prefix}")
if(DEFINED NA_CONFIG AND NOT "${NA_CONFIG}" STREQUAL "")
    list(APPEND install_cmd --config "${NA_CONFIG}")
endif()

execute_process(COMMAND ${install_cmd}
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "packaged-docs: cmake --install failed (rc=${rc})\n"
                        "  config: '${NA_CONFIG}'  build dir: ${NA_BUILD_DIR}\n${out}\n${err}")
endif()

# installed-relative path  ->  source-relative path it must be byte-identical to.
set(REQUIRED
    "${NA_DATAROOTDIR}/doc/naudio/copyright"                          "LICENSE"
    "${NA_DATAROOTDIR}/doc/naudio/THIRD_PARTY_NOTICES.md"             "THIRD_PARTY_NOTICES.md"
    "${NA_DATAROOTDIR}/doc/naudio/audio-streaming-protocol-v1.md"     "docs/audio-streaming-protocol-v1.md"
    "${NA_DATAROOTDIR}/doc/naudio/protocols.md"                       "docs/protocols.md"
    "${NA_DATAROOTDIR}/naudio/conformance/README.md"                  "conformance/README.md"
    "${NA_DATAROOTDIR}/naudio/conformance/vectors/vectors.ini"        "conformance/vectors/vectors.ini"
    "${NA_DATAROOTDIR}/naudio/conformance/vectors/vectors-v1_2.ini"   "conformance/vectors/vectors-v1_2.ini"
    "${NA_DATAROOTDIR}/naudio/conformance/vectors/vectors-v1_4.ini"   "conformance/vectors/vectors-v1_4.ini"
    "${NA_DATAROOTDIR}/naudio/conformance/tools/gen_vectors.py"       "conformance/tools/gen_vectors.py")

# --- arm 1: every required file is installed, and is the file it claims to be ---------------
set(failures "")
set(checked 0)
list(LENGTH REQUIRED n)
math(EXPR last "${n} / 2 - 1")
foreach(i RANGE ${last})
    math(EXPR ip "${i} * 2")
    math(EXPR is "${ip} + 1")
    list(GET REQUIRED ${ip} rel)
    list(GET REQUIRED ${is} srcrel)
    set(installed "${prefix}/${rel}")
    set(source "${NA_SOURCE_DIR}/${srcrel}")
    math(EXPR checked "${checked} + 1")
    if(NOT EXISTS "${installed}")
        list(APPEND failures "MISSING from the install tree: ${rel}")
    else()
        execute_process(COMMAND "${CMAKE_COMMAND}" -E compare_files "${installed}" "${source}"
                        RESULT_VARIABLE cmprc OUTPUT_QUIET ERROR_QUIET)
        if(NOT cmprc EQUAL 0)
            list(APPEND failures "DIFFERS from ${srcrel}: ${rel}")
        endif()
    endif()
endforeach()

# --- arm 2: the negative control — prove the predicate can say "missing" --------------------
# A file this project never installs. If EXISTS reports it present, the prefix or the path join
# is wrong and arm 1's silence proves nothing.
set(control "${prefix}/${NA_DATAROOTDIR}/doc/naudio/this-file-is-never-installed.md")
if(EXISTS "${control}")
    list(APPEND failures
         "NEGATIVE CONTROL FAILED: a file that is never installed was reported present, so the "
         "presence check is not discriminating and arm 1 measured nothing")
endif()

if(failures)
    string(REPLACE ";" "\n  " pretty "${failures}")
    message(FATAL_ERROR
        "packaged-docs: the install tree does not carry what a release must carry.\n  ${pretty}\n"
        "Prefix inspected: ${prefix}")
endif()

message(STATUS "packaged-docs: ${checked} required files installed and byte-identical; "
               "negative control fired correctly")
