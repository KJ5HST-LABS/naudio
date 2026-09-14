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
#
# ENTITLEMENTS. The hardened runtime restricts a process to the resources its signature claims,
# and microphone access is one of them (com.apple.security.device.audio-input). The two tools
# that open an INPUT device — na_audio_daemon and na_audio_source — are signed with
# packaging/macos/capture.entitlements; the playback client, na_wav_tap and libnaudio open no
# input and get none. Both halves are asserted on the signed bytes below. v1.0.0rc3, the release
# signed by hand, shipped exactly this on exactly these two (read back from its .pkg on
# 2026-09-02); the first CI-signed payload had dropped it.

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

set(_entitlements "${CMAKE_CURRENT_LIST_DIR}/../packaging/macos/capture.entitlements")
if(NOT EXISTS "${_entitlements}")
    message(FATAL_ERROR "codesign: entitlements file missing: ${_entitlements}")
endif()
set(_capture_tools na_audio_daemon na_audio_source)
set(_entitled 0)

foreach(_bin ${_machos})
    get_filename_component(_name "${_bin}" NAME)
    set(_ent_args "")
    if(_name IN_LIST _capture_tools)
        set(_ent_args --entitlements "${_entitlements}")
        math(EXPR _entitled "${_entitled} + 1")
    endif()
    execute_process(
        COMMAND codesign --force --sign "${CPACK_NAUDIO_CODESIGN_IDENTITY}"
                         --timestamp --options runtime ${_ent_args} "${_bin}"
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

    # Entitlements, both directions: the two capture tools must carry audio-input and the other
    # three must not. codesign prints the dictionary on stdout and nothing at all for a binary
    # that has none, so an empty result is the negative case, not a broken read.
    execute_process(COMMAND codesign -d --entitlements - "${_bin}"
                    OUTPUT_VARIABLE _ents ERROR_QUIET)
    if(_name IN_LIST _capture_tools)
        if(NOT _ents MATCHES "com\\.apple\\.security\\.device\\.audio-input")
            message(FATAL_ERROR
                "codesign: ${_name} does not carry the audio-input entitlement after signing "
                "with ${_entitlements}. A hardened-runtime capture tool without it captures "
                "silence when launched by launchd (measured 2026-09-02). Got:\n${_ents}")
        endif()
    elseif(_ents MATCHES "audio-input")
        message(FATAL_ERROR
            "codesign: ${_name} carries the audio-input entitlement and must not — only "
            "na_audio_daemon and na_audio_source open an input device. Got:\n${_ents}")
    endif()
endforeach()

# CARDINALITY again: exactly the two capture tools were entitled. A renamed tool would otherwise
# ship signed, hardened and silently unable to capture.
if(NOT _entitled EQUAL 2)
    message(FATAL_ERROR
        "codesign: expected to entitle exactly 2 capture tools (na_audio_daemon, "
        "na_audio_source), entitled ${_entitled}")
endif()

message(STATUS "codesign: all ${_n} payload binaries carry Developer ID + hardened runtime; "
               "${_entitled} capture tools carry audio-input")

# ---- Network Audio Service.app (issue #102) --------------------------------------------------
# Login Items names a launchd agent after the app its plist associates it with
# (AssociatedBundleIdentifiers) — but only when that app is Developer ID signed with the agent's
# Team ID. Measured 2026-09-13: Background Task Management silently dropped the association for
# an unsigned probe app and honoured it for a signed one, with the plist identical and valid in
# both cases. So the key in the plist is worth nothing unless the bundle it names is signed here.
#
# The bundle is not in the payload. tools/CMakeLists.txt keeps it out — pkgbuild auto-detects a
# .app in a payload and hands its placement to macOS Installer, which on a CI runner placed
# nothing — so the payload carries two flat files under share/naudio/ and the postinstall
# assembles them. What the payload CAN carry is the signature: for a bundle whose executable is a
# script, codesign writes it as ordinary files under Contents/_CodeSignature/ with no extended
# attributes, and a bundle reassembled from loose files with install(1) verifies --deep --strict
# (both measured, the second with a one-byte control that fails). This step assembles a temporary
# bundle from the staged pieces, signs it, verifies it, and copies the signature files back into
# the staging tree as share/naudio/naudio-control.codesig/*, which the postinstall installs into
# place beside the other two. A directory of data files is not a bundle and cannot trip the
# auto-detection.
#
# No entitlements: the bundle executable execs the daemon, and the exec'd image carries its own
# signature and audio-input entitlement (above). The seal covers Info.plist, which carries the
# project version — the roll happens before cpack, so the seal is always of the rolled file.

set(_app_script "")
set(_app_plist "")
set(_app_script_n 0)
set(_app_plist_n 0)
foreach(_f ${_candidates})
    get_filename_component(_name "${_f}" NAME)
    if(_name STREQUAL "naudio-control")
        set(_app_script "${_f}")
        math(EXPR _app_script_n "${_app_script_n} + 1")
    elseif(_name STREQUAL "naudio-control.Info.plist")
        set(_app_plist "${_f}")
        math(EXPR _app_plist_n "${_app_plist_n} + 1")
    endif()
endforeach()
# CARDINALITY (L230): exactly one of each, side by side. Zero means the app was not staged and
# Login Items would keep showing the developer's name; two means a tree this script does not
# understand. Neither is a payload to hand to CPack as signed.
if(NOT _app_script_n EQUAL 1 OR NOT _app_plist_n EQUAL 1)
    message(FATAL_ERROR
        "codesign: expected exactly one staged naudio-control and one naudio-control.Info.plist "
        "under ${_stage}; found ${_app_script_n} and ${_app_plist_n}. The control app cannot be "
        "signed, so Login Items would name the developer instead of the service.")
endif()
get_filename_component(_app_payload_dir "${_app_script}" DIRECTORY)
get_filename_component(_app_plist_dir "${_app_plist}" DIRECTORY)
if(NOT _app_payload_dir STREQUAL _app_plist_dir)
    message(FATAL_ERROR
        "codesign: the control app's pieces are staged in different directories "
        "(${_app_payload_dir} vs ${_app_plist_dir}); the postinstall reads both from one.")
endif()

# The daemon's Team ID, read off its signature — the relation Login Items depends on is asserted
# below against this, not against a literal.
set(_daemon "")
foreach(_bin ${_machos})
    get_filename_component(_name "${_bin}" NAME)
    if(_name STREQUAL "na_audio_daemon")
        set(_daemon "${_bin}")
    endif()
endforeach()
if(NOT _daemon)
    message(FATAL_ERROR "codesign: na_audio_daemon is not among the ${_n} signed Mach-O files")
endif()
execute_process(COMMAND codesign -dvv "${_daemon}" ERROR_VARIABLE _desc OUTPUT_QUIET)
string(REGEX MATCH "TeamIdentifier=([^\n]+)" _m "${_desc}")
set(_daemon_team "${CMAKE_MATCH_1}")
if(NOT _daemon_team)
    message(FATAL_ERROR "codesign: could not read a TeamIdentifier off ${_daemon}. Got:\n${_desc}")
endif()

# The bundle's names, read off the staged Info.plist. The on-disk name the postinstall assembles
# is what the Finder shows, and the Finder ignores a CFBundleDisplayName that differs from it
# (measured 2026-09-14), so the two keys must agree and the temporary bundles below are named
# after them — the same directory name the postinstall creates, not a literal of their own.
execute_process(COMMAND plutil -extract CFBundleName raw -o - "${_app_plist}"
                OUTPUT_VARIABLE _bundle_name ERROR_QUIET OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(COMMAND plutil -extract CFBundleDisplayName raw -o - "${_app_plist}"
                OUTPUT_VARIABLE _bundle_display ERROR_QUIET OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT _bundle_name)
    message(FATAL_ERROR "codesign: no CFBundleName in ${_app_plist}")
endif()
if(NOT _bundle_display STREQUAL _bundle_name)
    message(FATAL_ERROR
        "codesign: CFBundleDisplayName ('${_bundle_display}') differs from CFBundleName "
        "('${_bundle_name}') in ${_app_plist}; the Finder shows the on-disk name and Login Items "
        "the display name, so the service would carry two names")
endif()

# Assembled OUTSIDE the staging tree, deliberately: a .app inside it would be exactly the payload
# bundle tools/CMakeLists.txt keeps out. A sibling of the staging directory is never packaged.
# From the STAGED copies, so the seal is of the bytes that ship (install(PROGRAMS) has already
# made the script 0755 here; the mode is not sealed, the hash is).
set(_app_work "${_stage}.naudio-control-sign")
set(_app "${_app_work}/${_bundle_name}.app")
file(REMOVE_RECURSE "${_app_work}")
file(MAKE_DIRECTORY "${_app}/Contents/MacOS")
file(COPY "${_app_script}" DESTINATION "${_app}/Contents/MacOS")
file(COPY "${_app_plist}" DESTINATION "${_app}/Contents")
file(RENAME "${_app}/Contents/naudio-control.Info.plist" "${_app}/Contents/Info.plist")

execute_process(
    COMMAND codesign --force --sign "${CPACK_NAUDIO_CODESIGN_IDENTITY}"
                     --timestamp --options runtime "${_app}"
    RESULT_VARIABLE _rc ERROR_VARIABLE _err)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "codesign failed on ${_app}: ${_err}")
endif()

# On the artifact, as above. The identifier is read from the staged Info.plist and the Team ID
# from the signed daemon, so both are relations between shipped files rather than literals a
# rename could orphan.
execute_process(COMMAND plutil -extract CFBundleIdentifier raw -o - "${_app_plist}"
                OUTPUT_VARIABLE _bundle_id ERROR_QUIET OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT _bundle_id)
    message(FATAL_ERROR "codesign: no CFBundleIdentifier in ${_app_plist}")
endif()
execute_process(COMMAND codesign -dvv "${_app}" ERROR_VARIABLE _desc OUTPUT_QUIET)
if(NOT _desc MATCHES "Authority=Developer ID Application")
    message(FATAL_ERROR
        "codesign: ${_bundle_name}.app is not Developer ID Application signed after signing it. "
        "Got:\n${_desc}")
endif()
string(REGEX MATCH "\nIdentifier=([^\n]+)\n" _m "${_desc}")
if(NOT CMAKE_MATCH_1 STREQUAL _bundle_id)
    message(FATAL_ERROR
        "codesign: ${_bundle_name}.app was sealed as '${CMAKE_MATCH_1}', not as the bundle's "
        "CFBundleIdentifier (${_bundle_id}). Got:\n${_desc}")
endif()
string(REGEX MATCH "TeamIdentifier=([^\n]+)" _m "${_desc}")
if(NOT CMAKE_MATCH_1 STREQUAL _daemon_team)
    message(FATAL_ERROR
        "codesign: ${_bundle_name}.app's TeamIdentifier ('${CMAKE_MATCH_1}') is not the daemon's "
        "('${_daemon_team}'); Background Task Management drops the association unless they "
        "match. Got:\n${_desc}")
endif()
execute_process(COMMAND codesign --verify --deep --strict "${_app}"
                RESULT_VARIABLE _rc ERROR_VARIABLE _err)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "codesign: ${_bundle_name}.app does not verify after signing: ${_err}")
endif()

# Copy the signature back as loose payload files. Every file codesign wrote, not a fixed list:
# the set has varied across macOS releases (CodeRequirements-1 came and went), and a member
# left behind would ship a bundle that fails to verify with no local check firing.
set(_codesig_dst "${_app_payload_dir}/naudio-control.codesig")
file(GLOB _sigfiles "${_app}/Contents/_CodeSignature/*")
list(LENGTH _sigfiles _nsig)
if(_nsig LESS 3)
    message(FATAL_ERROR
        "codesign: ${_bundle_name}.app carries ${_nsig} file(s) under _CodeSignature; a sealed "
        "bundle carries at least CodeDirectory, CodeResources and CodeSignature")
endif()
file(REMOVE_RECURSE "${_codesig_dst}")
file(MAKE_DIRECTORY "${_codesig_dst}")
file(COPY ${_sigfiles} DESTINATION "${_codesig_dst}")

# PROVE THE COPY-BACK IS COMPLETE ON THE BYTES THAT SHIP: reassemble a second bundle the way the
# postinstall does — from the staged pieces plus the staged signature directory, nothing from the
# temporary bundle — and verify it. This is the postinstall's method run at packaging time.
set(_app2 "${_app_work}/reassembled/${_bundle_name}.app")
file(MAKE_DIRECTORY "${_app2}/Contents/MacOS" "${_app2}/Contents/_CodeSignature")
file(COPY "${_app_script}" DESTINATION "${_app2}/Contents/MacOS")
file(COPY "${_app_plist}" DESTINATION "${_app2}/Contents")
file(RENAME "${_app2}/Contents/naudio-control.Info.plist" "${_app2}/Contents/Info.plist")
file(GLOB _staged_sigs "${_codesig_dst}/*")
file(COPY ${_staged_sigs} DESTINATION "${_app2}/Contents/_CodeSignature")
execute_process(COMMAND codesign --verify --deep --strict "${_app2}"
                RESULT_VARIABLE _rc ERROR_VARIABLE _err)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR
        "codesign: a bundle reassembled from the staged pieces and ${_codesig_dst} does not "
        "verify — the postinstall would place an unverifiable app: ${_err}")
endif()
file(REMOVE_RECURSE "${_app_work}")

# CARDINALITY, once more: this step must have added no Mach-O to the payload. The notary would
# see a sixth binary this script never signed; the release gate counts five.
set(_n_after 0)
file(GLOB_RECURSE _after "${_stage}/*")
foreach(_f ${_after})
    if(IS_SYMLINK "${_f}" OR IS_DIRECTORY "${_f}")
        continue()
    endif()
    execute_process(COMMAND file -b "${_f}" OUTPUT_VARIABLE _kind ERROR_QUIET
                    OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(_kind MATCHES "Mach-O")
        math(EXPR _n_after "${_n_after} + 1")
    endif()
endforeach()
if(NOT _n_after EQUAL _n)
    message(FATAL_ERROR
        "codesign: the payload carried ${_n} Mach-O files before the control app was signed and "
        "${_n_after} after; the signature files must not be binaries")
endif()

message(STATUS "codesign: ${_bundle_name}.app sealed as ${_bundle_id} (TeamIdentifier "
               "${_daemon_team}); ${_nsig} signature files staged under "
               "${_codesig_dst}")
