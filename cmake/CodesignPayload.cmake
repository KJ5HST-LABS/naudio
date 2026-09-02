# SPDX-License-Identifier: LGPL-2.1-or-later
#
# naudio — sign the macOS payload binaries, in the one window where it survives.
#
# Copyright (C) 2025-2026 Terrell Deppe
#
# Run by CPack via CPACK_PRE_BUILD_SCRIPTS: after CPack has installed and stripped into its
# staging tree, and before the package is assembled.
#
# WHY NOT AT BUILD TIME. Apple's notary service validates every Mach-O INSIDE the .pkg, not
# just the wrapper productsign covers, and each needs a Developer ID Application signature, a
# secure timestamp and the hardened runtime. Signing after `cmake --build` does not survive:
# CMAKE_INSTALL_RPATH (CMakeLists.txt) makes install_name_tool rewrite the load commands, and
# CPACK_STRIP_FILES removes symbols. Either edit invalidates the signature, and nothing local
# says so — the failure surfaces ~3 minutes into notarization as `status: Invalid`, whose
# reason needs a separate `notarytool log <id>` call. See commit 2ae8cbb.
#
# This script therefore runs LAST, on the exact bytes that go into the package.

if(NOT CPACK_NAUDIO_CODESIGN_IDENTITY)
    message(FATAL_ERROR "CodesignPayload.cmake ran with no identity — it should not have been "
                        "registered. CPACK_PRE_BUILD_SCRIPTS is set only when "
                        "NAUDIO_CODESIGN_IDENTITY is non-empty.")
endif()

# CPack stages under CPACK_TEMPORARY_DIRECTORY. Resolve it defensively and say which name
# worked, because a silently-empty staging path would sign nothing and report success.
set(_stage "")
foreach(_candidate
        "${CPACK_TEMPORARY_DIRECTORY}"
        "${CPACK_TEMPORARY_INSTALL_DIRECTORY}"
        "${CPACK_TOPLEVEL_DIRECTORY}")
    if(_candidate AND IS_DIRECTORY "${_candidate}")
        set(_stage "${_candidate}")
        break()
    endif()
endforeach()
if(NOT _stage)
    message(FATAL_ERROR
        "codesign: no CPack staging directory found. Tried CPACK_TEMPORARY_DIRECTORY "
        "('${CPACK_TEMPORARY_DIRECTORY}'), CPACK_TEMPORARY_INSTALL_DIRECTORY "
        "('${CPACK_TEMPORARY_INSTALL_DIRECTORY}'), CPACK_TOPLEVEL_DIRECTORY "
        "('${CPACK_TOPLEVEL_DIRECTORY}').")
endif()
message(STATUS "codesign: staging tree ${_stage}")

# Every regular file, filtered to Mach-O by `file`. Globbing by extension would miss the
# extension-less executables, which are most of what ships.
file(GLOB_RECURSE _candidates "${_stage}/*")
set(_machos "")
foreach(_f ${_candidates})
    if(IS_SYMLINK "${_f}" OR IS_DIRECTORY "${_f}")
        continue()
    endif()
    execute_process(COMMAND file -b "${_f}" OUTPUT_VARIABLE _kind ERROR_QUIET
                    OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(_kind MATCHES "Mach-O")
        list(APPEND _machos "${_f}")
    endif()
endforeach()

# CARDINALITY (L230). The package ships four executables plus libnaudio; a glob that found
# nothing, or suspiciously little, is a broken harness reporting success. Refuse rather than
# hand CPack an unsigned payload that looks signed because this script exited 0.
list(LENGTH _machos _n)
if(_n LESS 5)
    message(FATAL_ERROR
        "codesign: found only ${_n} Mach-O files under ${_stage}; the macOS payload carries at "
        "least 5 (na_audio_daemon, na_wav_tap, na_audio_source, na_c_play_to_speakers and "
        "libnaudio). Refusing to sign a payload this script cannot see.")
endif()
message(STATUS "codesign: ${_n} Mach-O binaries with '${CPACK_NAUDIO_CODESIGN_IDENTITY}'")

foreach(_bin ${_machos})
    execute_process(
        COMMAND codesign --force --sign "${CPACK_NAUDIO_CODESIGN_IDENTITY}"
                         --timestamp --options runtime "${_bin}"
        RESULT_VARIABLE _rc ERROR_VARIABLE _err)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "codesign failed on ${_bin}: ${_err}")
    endif()

    # ASSERT ON THE ARTIFACT, NOT THE EXIT CODE. codesign returning 0 is not evidence the
    # signature is the one the notary requires: an ad-hoc or timestamp-less signature also
    # exits 0, and that difference is exactly what rejected v1.0.0rc5.
    execute_process(COMMAND codesign -dvv "${_bin}"
                    ERROR_VARIABLE _desc OUTPUT_QUIET)
    if(NOT _desc MATCHES "Authority=Developer ID Application")
        message(FATAL_ERROR
            "codesign: ${_bin} is not Developer ID Application signed after signing it. "
            "The notary would reject this package. Got:\n${_desc}")
    endif()
    if(NOT _desc MATCHES "flags=0x[0-9a-f]*[^ ]*runtime")
        message(FATAL_ERROR
            "codesign: ${_bin} lacks the hardened runtime after signing. Got:\n${_desc}")
    endif()
endforeach()

message(STATUS "codesign: all ${_n} payload binaries carry Developer ID + hardened runtime")
