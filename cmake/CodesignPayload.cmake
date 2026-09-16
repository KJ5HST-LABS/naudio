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

# CARDINALITY (L230). The package ships four executables — the daemon twice, since issue #105:
# once in bin/ and once beside the control app's pieces as the bundle's executable — plus
# libnaudio; a glob that found nothing, or suspiciously little, is a broken harness reporting
# success. Refuse rather than hand CPack an unsigned payload that looks signed because this
# script exited 0.
list(LENGTH _machos _n)
if(_n LESS 6)
    message(FATAL_ERROR
        "codesign: found only ${_n} Mach-O files under ${_stage}; the macOS payload carries at "
        "least 6 (na_audio_daemon twice, na_wav_tap, na_audio_source, na_c_play_to_speakers and "
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

# CARDINALITY again: exactly the capture tools were entitled — the daemon's two copies and
# na_audio_source. A renamed tool would otherwise ship signed, hardened and silently unable to
# capture.
if(NOT _entitled EQUAL 3)
    message(FATAL_ERROR
        "codesign: expected to entitle exactly 3 capture tools (na_audio_daemon twice, "
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
# nothing — so the payload carries flat files under share/naudio/ and the postinstall assembles
# them. What the payload CAN carry is the signature. Since issue #105 the bundle's executable is
# the daemon itself (a second staged copy of na_audio_daemon, beside the plist and the icon), so
# the signature has two parts: the executable's own, which codesign embeds in the Mach-O, and the
# bundle's seal, which it writes as ordinary files under Contents/_CodeSignature/ with no
# extended attributes. Both survive reassembly by install(1) and verify --deep --strict
# (measured, with controls that fail). This step assembles a temporary bundle from the staged
# pieces, signs it with the capture entitlements and the hardened runtime — the executable is
# the process that opens the microphone now — verifies it, writes the bundle-signed executable
# back over the staged copy, copies the seal files back as share/naudio/app.codesig/*, and
# proves the pieces reassemble into a bundle that verifies. A directory of data files is not a
# bundle and cannot trip the auto-detection.
#
# THEN THE SEALED BUNDLE GOES INTO THE PAYLOAD AS A ZIP, AND THE LOOSE EXECUTABLE COMES OUT
# (measured on the v1.0.0rc9 tag, 2026-09-16 — the first notarization since #105). The notary
# validates every Mach-O in a package payload STANDALONE, and a bundle's executable is valid
# only inside its bundle: its code directory binds the bundle's Info.plist, so the loose copy
# is "The signature of the binary is invalid" to the notary, exactly as it is to the kernel,
# which kills it at exec (the release's tarball gate found the same thing the same hour). A
# tool-signed copy cannot stand in for it either: the seal binds the executable both ways
# ("code has no resources but signature indicates they must be present"). So a signed payload
# carries share/naudio/app.zip — the bundle as ditto packs it, the form Apple's own tools
# exchange signed bundles in, seal and modes intact — and the postinstall extracts it with
# ditto instead of assembling the pieces. The plist, the icon and the seal files stay beside
# it as the record the gates read; an unsigned build never runs this script and its
# postinstall assembles the unsigned bundle from those pieces as before. The Mach-O count
# therefore drops by exactly one here, and the cardinality check at the end says so.
#
# The seal covers Info.plist, which carries the project version — the roll happens before
# cpack, so the seal is always of the rolled file.

set(_app_exec "")
set(_app_plist "")
set(_app_icon "")
set(_app_exec_n 0)
set(_app_plist_n 0)
set(_app_icon_n 0)
foreach(_f ${_candidates})
    get_filename_component(_name "${_f}" NAME)
    if(_name STREQUAL "naudio-control.Info.plist")
        set(_app_plist "${_f}")
        math(EXPR _app_plist_n "${_app_plist_n} + 1")
    elseif(_name STREQUAL "naudio.icns")
        set(_app_icon "${_f}")
        math(EXPR _app_icon_n "${_app_icon_n} + 1")
    endif()
endforeach()
if(NOT _app_plist_n EQUAL 1 OR NOT _app_icon_n EQUAL 1)
    message(FATAL_ERROR
        "codesign: expected exactly one staged naudio-control.Info.plist and one naudio.icns "
        "under ${_stage}; found ${_app_plist_n} and ${_app_icon_n}. The control app cannot be "
        "signed, so Login Items would name the developer instead of the service.")
endif()
get_filename_component(_app_payload_dir "${_app_plist}" DIRECTORY)
get_filename_component(_app_icon_dir "${_app_icon}" DIRECTORY)
if(NOT _app_payload_dir STREQUAL _app_icon_dir)
    message(FATAL_ERROR
        "codesign: the control app's pieces are staged in different directories "
        "(${_app_payload_dir}, ${_app_icon_dir}); the postinstall reads them all from one.")
endif()
# The executable: the daemon copy staged BESIDE the plist, told apart from the bin/ copy by its
# directory. Exactly one there, and the name is read off the plist's CFBundleExecutable rather
# than assumed, so the file the postinstall places is the one the bundle will look for.
execute_process(COMMAND plutil -extract CFBundleExecutable raw -o - "${_app_plist}"
                OUTPUT_VARIABLE _app_exec_name ERROR_QUIET OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT _app_exec_name)
    message(FATAL_ERROR "codesign: no CFBundleExecutable in ${_app_plist}")
endif()
foreach(_bin ${_machos})
    get_filename_component(_name "${_bin}" NAME)
    get_filename_component(_dir "${_bin}" DIRECTORY)
    if(_name STREQUAL _app_exec_name AND _dir STREQUAL _app_payload_dir)
        set(_app_exec "${_bin}")
        math(EXPR _app_exec_n "${_app_exec_n} + 1")
    endif()
endforeach()
# CARDINALITY (L230): exactly one of each, side by side. Zero means the app was not staged and
# Login Items would keep showing the developer's name — or, for the executable, that the agent
# would be registered against a program the install never places; two means a tree this script
# does not understand. Neither is a payload to hand to CPack as signed. The icon (issue #103)
# is a piece in its own right: the seal covers Contents/Resources, so a bundle sealed with it
# and assembled without it does not verify, and one sealed without it has no icon anywhere —
# either way the staged set has to be the complete one.
if(NOT _app_exec_n EQUAL 1)
    message(FATAL_ERROR
        "codesign: expected exactly one Mach-O named '${_app_exec_name}' beside ${_app_plist} "
        "(the bundle's executable, tools/CMakeLists.txt's second install of the daemon); found "
        "${_app_exec_n}. The launchd agent names it, so the service would not start.")
endif()
# The icon goes into the bundle under the name Info.plist gives it, and the postinstall
# installs the staged file under its own name — so the two names must be one, asserted as a
# relation between the staged plist and the staged file rather than as a literal.
execute_process(COMMAND plutil -extract CFBundleIconFile raw -o - "${_app_plist}"
                OUTPUT_VARIABLE _icon_name ERROR_QUIET OUTPUT_STRIP_TRAILING_WHITESPACE)
get_filename_component(_app_icon_file "${_app_icon}" NAME)
if(NOT _icon_name STREQUAL _app_icon_file)
    message(FATAL_ERROR
        "codesign: ${_app_plist} names its icon '${_icon_name}' but the staged icon is "
        "'${_app_icon_file}'; the postinstall would place a file the bundle does not look for")
endif()

# The bin/ daemon's Team ID, read off its signature — the relation Login Items depends on is
# asserted below against this, not against a literal. The bin/ copy specifically: the other one
# is about to be re-signed as the bundle, and the relation is between the bundle and the tool
# an operator runs from a terminal.
set(_daemon "")
set(_daemon_n 0)
foreach(_bin ${_machos})
    get_filename_component(_name "${_bin}" NAME)
    if(_name STREQUAL "na_audio_daemon" AND NOT _bin STREQUAL _app_exec)
        set(_daemon "${_bin}")
        math(EXPR _daemon_n "${_daemon_n} + 1")
    endif()
endforeach()
if(NOT _daemon_n EQUAL 1)
    message(FATAL_ERROR
        "codesign: expected exactly one na_audio_daemon among the ${_n} signed Mach-O files "
        "besides the bundle's copy; found ${_daemon_n}")
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
# From the STAGED copies, so the seal is of the bytes that ship (the executable is the stripped,
# RPATH-rewritten, already-signed staged file; the mode is not sealed, the hash is).
set(_app_work "${_stage}.naudio-control-sign")
set(_app "${_app_work}/${_bundle_name}.app")
file(REMOVE_RECURSE "${_app_work}")
file(MAKE_DIRECTORY "${_app}/Contents/MacOS" "${_app}/Contents/Resources")
file(COPY "${_app_exec}" DESTINATION "${_app}/Contents/MacOS")
file(COPY "${_app_plist}" DESTINATION "${_app}/Contents")
file(RENAME "${_app}/Contents/naudio-control.Info.plist" "${_app}/Contents/Info.plist")
file(COPY "${_app_icon}" DESTINATION "${_app}/Contents/Resources")

# With the capture entitlements and the hardened runtime, like the bin/ copy above: the bundle's
# executable IS the process that opens the microphone now, and a hardened daemon without the
# entitlement captures silence under launchd (measured 2026-09-02).
execute_process(
    COMMAND codesign --force --sign "${CPACK_NAUDIO_CODESIGN_IDENTITY}"
                     --timestamp --options runtime --entitlements "${_entitlements}" "${_app}"
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
# The executable, on the artifact: the hardened runtime and the audio-input entitlement went
# onto the bundle's main executable, or the service captures silence (issue #105 is exactly the
# report a silent capture produces, from the other cause).
set(_app_exec_signed "${_app}/Contents/MacOS/${_app_exec_name}")
execute_process(COMMAND codesign -dvv "${_app_exec_signed}" ERROR_VARIABLE _desc OUTPUT_QUIET)
if(NOT _desc MATCHES "flags=0x[0-9a-f]*[^ ]*runtime")
    message(FATAL_ERROR
        "codesign: ${_bundle_name}.app's executable lacks the hardened runtime after signing. "
        "Got:\n${_desc}")
endif()
execute_process(COMMAND codesign -d --entitlements - "${_app_exec_signed}"
                OUTPUT_VARIABLE _ents ERROR_QUIET)
if(NOT _ents MATCHES "com\\.apple\\.security\\.device\\.audio-input")
    message(FATAL_ERROR
        "codesign: ${_bundle_name}.app's executable does not carry the audio-input entitlement "
        "after signing with ${_entitlements}; the service would capture silence. Got:\n${_ents}")
endif()

# Write the bundle-signed executable back over the staged copy. The Mach-O carries its own
# signature, and the one it carried a moment ago named it 'na_audio_daemon'; the bundle's seal
# expects the bytes signed as the bundle. Removed first and compared after, rather than trusting
# file(COPY)'s timestamp shortcut (L230).
file(REMOVE "${_app_exec}")
file(COPY "${_app_exec_signed}" DESTINATION "${_app_payload_dir}")
execute_process(COMMAND cmp -s "${_app_exec_signed}" "${_app_exec}" RESULT_VARIABLE _rc)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR
        "codesign: the staged ${_app_exec} is not the bundle-signed executable after the "
        "write-back")
endif()

# Copy the seal back as loose payload files. Every file codesign wrote, not a fixed list: the
# set has varied across macOS releases (CodeRequirements-1 came and went), and a member left
# behind would ship a bundle that fails to verify with no local check firing.
set(_codesig_dst "${_app_payload_dir}/app.codesig")
file(GLOB _sigfiles "${_app}/Contents/_CodeSignature/*")
list(LENGTH _sigfiles _nsig)
if(_nsig LESS 1)
    message(FATAL_ERROR
        "codesign: ${_bundle_name}.app carries nothing under _CodeSignature; a sealed bundle "
        "carries at least CodeResources")
endif()
file(REMOVE_RECURSE "${_codesig_dst}")
file(MAKE_DIRECTORY "${_codesig_dst}")
file(COPY ${_sigfiles} DESTINATION "${_codesig_dst}")

# PROVE THE COPY-BACK IS COMPLETE ON THE BYTES THAT SHIP: reassemble a second bundle the way the
# postinstall does — from the staged pieces plus the staged seal directory, nothing from the
# temporary bundle — and verify it. This is the postinstall's method run at packaging time.
function(_naudio_reassemble _dst _exec)
    file(REMOVE_RECURSE "${_dst}")
    file(MAKE_DIRECTORY "${_dst}/Contents/MacOS" "${_dst}/Contents/Resources"
                        "${_dst}/Contents/_CodeSignature")
    file(COPY "${_exec}" DESTINATION "${_dst}/Contents/MacOS")
    file(COPY "${_app_plist}" DESTINATION "${_dst}/Contents")
    file(RENAME "${_dst}/Contents/naudio-control.Info.plist" "${_dst}/Contents/Info.plist")
    file(COPY "${_app_icon}" DESTINATION "${_dst}/Contents/Resources")
    file(GLOB _staged_sigs "${_codesig_dst}/*")
    file(COPY ${_staged_sigs} DESTINATION "${_dst}/Contents/_CodeSignature")
endfunction()
set(_app2 "${_app_work}/reassembled/${_bundle_name}.app")
_naudio_reassemble("${_app2}" "${_app_exec}")
execute_process(COMMAND codesign --verify --deep --strict "${_app2}"
                RESULT_VARIABLE _rc ERROR_VARIABLE _err)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR
        "codesign: a bundle reassembled from the staged pieces and ${_codesig_dst} does not "
        "verify — the postinstall would place an unverifiable app: ${_err}")
endif()
# CAN-FAIL CONTROLS (L222). (1) The same reassembly MINUS the icon must NOT verify: that is what
# proves the seal covers Contents/Resources — that the verification above would have caught a
# postinstall that forgot the icon, and that the postinstall's copy must be the sealed bytes.
file(REMOVE "${_app2}/Contents/Resources/${_app_icon_file}")
execute_process(COMMAND codesign --verify --deep --strict "${_app2}"
                RESULT_VARIABLE _rc ERROR_QUIET OUTPUT_QUIET)
if(_rc EQUAL 0)
    message(FATAL_ERROR
        "codesign: control broken — ${_bundle_name}.app verified with its icon removed, so the "
        "seal does not cover Contents/Resources and the reassembly proof above says nothing "
        "about the icon")
endif()
# (2) The same reassembly with the bin/ daemon as the executable — the same program, signed as a
# tool rather than as this bundle — must NOT verify either: that is what proves the write-back
# above was needed, and that a postinstall placing the wrong copy would be caught. A verify that
# passed with either copy would be proving nothing about the executable.
set(_app3 "${_app_work}/reassembled-bin/${_bundle_name}.app")
_naudio_reassemble("${_app3}" "${_daemon}")
execute_process(COMMAND codesign --verify --deep --strict "${_app3}"
                RESULT_VARIABLE _rc ERROR_QUIET OUTPUT_QUIET)
if(_rc EQUAL 0)
    message(FATAL_ERROR
        "codesign: control broken — ${_bundle_name}.app verified with the bin/ daemon as its "
        "executable, so the seal does not bind the executable and the write-back proof above "
        "says nothing")
endif()

# The payload's form of the bundle: a ditto zip of the sealed bundle (the temporary one, whose
# every byte the checks above measured), beside the pieces. Proved on the artifact by
# extracting it again, verifying the extracted bundle and comparing its executable to the
# bundle-signed bytes — a zip that dropped the seal or a mode would pass ditto and fail here.
set(_app_zip "${_app_payload_dir}/app.zip")
file(REMOVE "${_app_zip}")
execute_process(COMMAND ditto -c -k --keepParent "${_app}" "${_app_zip}"
                RESULT_VARIABLE _rc ERROR_VARIABLE _err)
if(NOT _rc EQUAL 0 OR NOT EXISTS "${_app_zip}")
    message(FATAL_ERROR "codesign: ditto could not zip ${_bundle_name}.app into the payload: ${_err}")
endif()
set(_unzipped "${_app_work}/unzipped")
file(REMOVE_RECURSE "${_unzipped}")
execute_process(COMMAND ditto -x -k "${_app_zip}" "${_unzipped}"
                RESULT_VARIABLE _rc ERROR_VARIABLE _err)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "codesign: ditto could not extract ${_app_zip} again: ${_err}")
endif()
execute_process(COMMAND codesign --verify --deep --strict "${_unzipped}/${_bundle_name}.app"
                RESULT_VARIABLE _rc ERROR_VARIABLE _err)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR
        "codesign: ${_bundle_name}.app does not verify after a trip through ${_app_zip}: ${_err}")
endif()
execute_process(COMMAND cmp -s "${_unzipped}/${_bundle_name}.app/Contents/MacOS/${_app_exec_name}"
                        "${_app_exec}" RESULT_VARIABLE _rc)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR
        "codesign: the executable inside ${_app_zip} is not the bundle-signed one")
endif()
# Now the loose copy goes: the notary would reject it, and nothing installs it any more.
file(REMOVE "${_app_exec}")
if(EXISTS "${_app_exec}")
    message(FATAL_ERROR "codesign: could not remove the loose ${_app_exec} from the payload")
endif()
file(REMOVE_RECURSE "${_app_work}")

# CARDINALITY, once more: this step must have removed exactly ONE Mach-O from the payload — the
# bundle's executable, inside app.zip now — and added none. The notary would see a binary this
# script never signed; the release gates count five loose plus the one in the bundle.
math(EXPR _n_expected "${_n} - 1")
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
if(NOT _n_after EQUAL _n_expected)
    message(FATAL_ERROR
        "codesign: the payload carried ${_n} Mach-O files before the control app was signed and "
        "${_n_after} after; expected ${_n_expected} — the bundle's executable inside app.zip "
        "and no other change (the signature files must not be binaries)")
endif()

message(STATUS "codesign: ${_bundle_name}.app sealed as ${_bundle_id} (TeamIdentifier "
               "${_daemon_team}); ${_nsig} signature files staged under "
               "${_codesig_dst}; the sealed bundle carried as ${_app_zip}, "
               "${_n_after} loose Mach-O files remain")
