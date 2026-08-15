# SPDX-License-Identifier: LGPL-2.1-or-later
#
# naudio — the spec-citation gate. Driven by the `naudio_spec_citations` ctest arm; see the
# add_test() comment in the root CMakeLists.txt for why it exists (issue #88 item 7, from #78).
#
# Run as: cmake -D NA_SOURCE_DIR=... -D NA_WORK_DIR=... -P check.cmake
#
# TWO HALVES, and neither is sufficient alone.
#
#   (1) ENUMERATION. The tree is scanned for citations into the spec and the result must equal the
#       registered set in citations.txt. A citation added without a manifest line fails here rather
#       than going quietly uncovered — the failure mode a manifest-only checker has, where the
#       checklist slowly stops describing what it scores.
#   (2) ANCHORS. Each registered citation must resolve to a line still containing its recorded
#       anchor text. A line number alone cannot rot loudly; an anchor can.
#
# TWO ARMS, and the second is not decoration — same reason as the C99 gate next door. This gate
# passes by finding nothing wrong, which is the shape most able to pass while measuring nothing: a
# glob that matched no files, a spec read as one long line, or a manifest that parsed to zero
# entries all report "clean" exactly as a correct tree does. So arm 2 re-runs the anchor half
# against a FIXTURE spec with one line inserted, and every single-line citation must be seen to
# FAIL. Only then does arm 1's silence mean anything. (#88 item 7 asks for exactly this, and names
# the trap: "An absence-check that has never failed is the trap one level up.")

foreach(var NA_SOURCE_DIR NA_WORK_DIR)
    if(NOT DEFINED ${var} OR "${${var}}" STREQUAL "")
        message(FATAL_ERROR "spec-citations: ${var} was not passed to the script")
    endif()
endforeach()

set(NA_SPEC_REL "docs/audio-streaming-protocol-v1.md")
set(spec "${NA_SOURCE_DIR}/${NA_SPEC_REL}")
set(manifest "${NA_SOURCE_DIR}/tests/spec-citations/citations.txt")
foreach(f "${spec}" "${manifest}")
    if(NOT EXISTS "${f}")
        message(FATAL_ERROR "spec-citations: ${f} is missing")
    endif()
endforeach()

# The spec is named by FILE NAME, not by path, because two of the three citation shapes drop the
# directory. Everything below keys on this one string.
get_filename_component(NA_SPEC_NAME "${NA_SPEC_REL}" NAME)

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

# Read a file into a ;-list of lines, 1 element per line, blanks preserved.
#
# file(STRINGS) is the obvious tool and the wrong one: it drops empty lines, so every line number
# after the first blank would be short by one — a line-number checker that cannot count lines.
#
# Splitting on "\n" by hand has the SAME defect one layer down, and BOTH of the ways it goes wrong
# were measured here rather than reasoned about, because both are silent:
#
#   * CMake drops EMPTY elements when a list is expanded, so a blank line vanishes exactly as it
#     does under file(STRINGS). Every element therefore carries a leading sentinel and is never
#     empty; na_line() strips it again.
#   * CMake's list parser treats an UNMATCHED '[' as opening a bracket and stops honouring ';'
#     separators until it is closed. This spec says "bytes [0,2)" on line 151, and that one line
#     swallowed the remaining 410: the 560-line file read as 151 lines, with element 150 a single
#     29 KB blob. Nothing errored. So '[', ']' and '\' are parked as placeholders across the split
#     and restored by na_line(); ';' is parked the same way rather than backslash-escaped, so that
#     what comes back out is the literal text and not an escaped form of it.
#
# CRLF is stripped for the Windows checkout, where core.autocrlf leaves a trailing \r on every line
# and would defeat a literal anchor comparison that ends a line.
set(NA_LINE_SENTINEL "~")

function(na_read_lines path out)
    file(READ "${path}" _text)
    string(REPLACE "\\" "@NA_BS@" _text "${_text}")
    string(REPLACE "[" "@NA_LB@" _text "${_text}")
    string(REPLACE "]" "@NA_RB@" _text "${_text}")
    string(REPLACE ";" "@NA_SC@" _text "${_text}")
    string(REPLACE "\r\n" "\n" _text "${_text}")
    string(REPLACE "\n" ";${NA_LINE_SENTINEL}" _text "${_text}")
    set(${out} "${NA_LINE_SENTINEL}${_text}" PARENT_SCOPE)
endfunction()

# Strip the sentinel off one element read by na_read_lines.
#
# NOT string(REGEX REPLACE "^." "") — CMake re-anchors ^ after each match and walks the whole
# string, so that spelling deletes every character rather than the first. It returned "" for every
# line here, which the manifest's zero-entries guard caught immediately; a gate without that guard
# would have been reading blank lines and comparing them to anchors.
function(na_line raw out)
    string(LENGTH "${raw}" _n)
    if(_n LESS_EQUAL 1)
        set(${out} "" PARENT_SCOPE)
    else()
        string(SUBSTRING "${raw}" 1 -1 _l)
        string(REPLACE "@NA_SC@" ";" _l "${_l}")
        string(REPLACE "@NA_RB@" "]" _l "${_l}")
        string(REPLACE "@NA_LB@" "[" _l "${_l}")
        string(REPLACE "@NA_BS@" "\\" _l "${_l}")
        set(${out} "${_l}" PARENT_SCOPE)
    endif()
endfunction()

# Split "FROM-TO" or "N" into two numbers. Returns 0/0 for anything else.
function(na_parse_target target out_from out_to)
    if(target MATCHES "^([0-9]+)-([0-9]+)$")
        set(${out_from} "${CMAKE_MATCH_1}" PARENT_SCOPE)
        set(${out_to} "${CMAKE_MATCH_2}" PARENT_SCOPE)
    elseif(target MATCHES "^([0-9]+)$")
        set(${out_from} "${target}" PARENT_SCOPE)
        set(${out_to} "${target}" PARENT_SCOPE)
    else()
        set(${out_from} "0" PARENT_SCOPE)
        set(${out_to} "0" PARENT_SCOPE)
    endif()
endfunction()

# Check every registered citation's anchor against one spec file. Appends human-readable failure
# strings to ${out}. Taking the spec path as a parameter is what lets arm 2 re-run the identical
# logic against the fixture — a control that exercised a *copy* of this code would prove nothing
# about the code that runs in arm 1.
function(na_check_anchors spec_path entries out)
    na_read_lines("${spec_path}" _lines)
    list(LENGTH _lines _count)
    set(_fails "")
    foreach(_entry IN LISTS entries)
        string(REPLACE "|" ";" _parts "${_entry}")
        list(GET _parts 0 _file)
        list(GET _parts 1 _target)
        list(GET _parts 2 _anchor)
        na_parse_target("${_target}" _from _to)
        if(_from EQUAL 0)
            list(APPEND _fails "${_file} -> :${_target}  (unparseable spec target)")
            continue()
        endif()
        if(_to GREATER _count)
            list(APPEND _fails
                 "${_file} -> :${_target}  cites past the end of the spec (${_count} lines)")
            continue()
        endif()
        set(_hit FALSE)
        foreach(_n RANGE ${_from} ${_to})
            math(EXPR _idx "${_n} - 1")
            list(GET _lines ${_idx} _raw)
            na_line("${_raw}" _line)
            string(FIND "${_line}" "${_anchor}" _pos)
            if(NOT _pos EQUAL -1)
                set(_hit TRUE)
                break()
            endif()
        endforeach()
        if(NOT _hit)
            math(EXPR _idx "${_from} - 1")
            list(GET _lines ${_idx} _raw)
            na_line("${_raw}" _line)
            string(LENGTH "${_line}" _len)
            if(_len GREATER 90)
                string(SUBSTRING "${_line}" 0 90 _shown)
            else()
                set(_shown "${_line}")
            endif()
            # The quoted excerpt is arbitrary file text, and these failures are accumulated in a
            # LIST — so a ';' in the excerpt splits one failure into several elements. That is not
            # only ugly: arm 2 asserts on the NUMBER of failures, and the first version of this
            # gate reported 12 of them from 10 registered citations, because one shifted line
            # ("first submit claims a free channel; the same owner keeps it...") carried three.
            # A count that can exceed its own input is a broken instrument, so the separator is
            # removed from the excerpt here, where it is display text and nothing else.
            string(REPLACE ";" "," _shown "${_shown}")
            list(APPEND _fails
                 "${_file} -> :${_target}  anchor \"${_anchor}\" is NOT there. The line now reads: ${_shown}")
        endif()
    endforeach()
    set(${out} "${_fails}" PARENT_SCOPE)
endfunction()

# ---------------------------------------------------------------------------
# Load the manifest
# ---------------------------------------------------------------------------
na_read_lines("${manifest}" manifest_lines)
set(entries "")
set(keys "")
foreach(raw IN LISTS manifest_lines)
    na_line("${raw}" line)
    string(STRIP "${line}" line)
    if(line STREQUAL "" OR line MATCHES "^#")
        continue()
    endif()
    string(REPLACE "|" ";" parts "${line}")
    list(LENGTH parts nparts)
    if(NOT nparts EQUAL 3)
        message(FATAL_ERROR
            "spec-citations: citations.txt line is not <file> | <spec> | <anchor>:\n  ${line}")
    endif()
    list(GET parts 0 e_file)
    list(GET parts 1 e_target)
    list(GET parts 2 e_anchor)
    string(STRIP "${e_file}" e_file)
    string(STRIP "${e_target}" e_target)
    string(STRIP "${e_anchor}" e_anchor)
    if(NOT EXISTS "${NA_SOURCE_DIR}/${e_file}")
        message(FATAL_ERROR
            "spec-citations: citations.txt registers ${e_file}, which does not exist. A renamed or "
            "deleted citing file leaves its entry behind; move or drop the entry.")
    endif()
    list(APPEND entries "${e_file}|${e_target}|${e_anchor}")
    list(APPEND keys "${e_file}|${e_target}")
endforeach()

list(LENGTH entries entry_count)
if(entry_count EQUAL 0)
    message(FATAL_ERROR "spec-citations: citations.txt parsed to zero entries — the gate would "
                        "pass by checking nothing.")
endif()

set(dupes "${keys}")
list(REMOVE_DUPLICATES dupes)
list(LENGTH dupes dupe_count)
if(NOT dupe_count EQUAL entry_count)
    message(FATAL_ERROR "spec-citations: citations.txt registers the same (file, spec line) twice. "
                        "One entry per claim — several occurrences of one target in one file are "
                        "one entry.")
endif()

# ---------------------------------------------------------------------------
# Arm 1a — ENUMERATE the tree, and require it to equal the registered set
#
# Shapes 1 and 2 (`docs/<spec>:NNN` and a bare `<spec>:NNN`) are found anywhere in a file: the file
# name is its own anchor, and one of them lives inside a C++ string literal rather than a comment
# (tests/net/test_server.cpp), so restricting the scan to comments would miss it.
#
# Shape 3 — a trailing `:NNN` with no file name at all — has no such anchor, and this is where a
# naive scan dies: bare `:NNN` is used about thirty times in this tree for SOURCE line numbers and
# only three times for the spec. So shape 3 counts only inside a run of comment lines that names
# the spec (by file name, or by the bare word "spec"). Measured against the tree at the time of
# writing, that rule returns exactly the 3 real citations and no false positives. The bare-word
# clause is load-bearing rather than belt-and-braces: it is the only thing that finds
# `(spec :323)` in src/net/AudioMixer.cpp — which was the one citation actually rotted when this
# gate was written, missed by the hand repair pass in 1313d0e/06fa36a precisely because no
# path-anchored grep can see it.
# ---------------------------------------------------------------------------
set(scan_dirs src include tests tools examples conformance)
set(scan_globs "")
foreach(d IN LISTS scan_dirs)
    foreach(ext c h cpp hpp cc py rs java sh)
        list(APPEND scan_globs "${NA_SOURCE_DIR}/${d}/*.${ext}")
    endforeach()
endforeach()
file(GLOB_RECURSE scan_files ${scan_globs})
list(LENGTH scan_files scan_count)
if(scan_count EQUAL 0)
    message(FATAL_ERROR "spec-citations: the source scan matched no files — the enumeration half "
                        "would report an empty tree as agreement.")
endif()

set(found "")
set(found_where "")
foreach(path IN LISTS scan_files)
    file(RELATIVE_PATH rel "${NA_SOURCE_DIR}" "${path}")
    na_read_lines("${path}" flines)
    set(lineno 0)
    set(block_text "")          # accumulated text of the comment run we are inside
    set(block_pending "")       # bare :NNN seen in that run, as "lineno|target"
    foreach(raw IN LISTS flines)
        na_line("${raw}" line)
        math(EXPR lineno "${lineno} + 1")

        # -- shapes 1 and 2: the file name carries the citation, so context does not matter.
        string(REGEX MATCHALL "${NA_SPEC_NAME}:[0-9][0-9]*(-[0-9][0-9]*)?" named "${line}")
        foreach(m IN LISTS named)
            string(REGEX REPLACE "^.*:" "" target "${m}")
            string(REGEX MATCH "[0-9][0-9]*(-[0-9][0-9]*)?$" target "${m}")
            list(APPEND found "${rel}|${target}")
            list(APPEND found_where "${rel}:${lineno} -> :${target}")
        endforeach()

        # -- shape 3: only inside a comment run that names the spec.
        string(STRIP "${line}" stripped)
        if(stripped MATCHES "^(//|\\*|/\\*|#)")
            set(block_text "${block_text} ${line}")
            # A bare :NNN is one NOT preceded by a name character, which is what keeps
            # `<spec>.md:325` and `AudioStreamServer.cpp:209` out of this shape by construction.
            string(REGEX MATCHALL "[^A-Za-z0-9_./]:[0-9][0-9]*(-[0-9][0-9]*)?" bare "${line}")
            foreach(m IN LISTS bare)
                string(REGEX MATCH "[0-9][0-9]*(-[0-9][0-9]*)?$" target "${m}")
                list(APPEND block_pending "${lineno}|${target}")
            endforeach()
        else()
            # The run ended: keep its bare citations only if the run named the spec.
            if(block_pending)
                set(padded " ${block_text} ")
                if(padded MATCHES "${NA_SPEC_NAME}" OR padded MATCHES "[^A-Za-z][Ss][Pp][Ee][Cc][^A-Za-z]")
                    foreach(p IN LISTS block_pending)
                        string(REPLACE "|" ";" pp "${p}")
                        list(GET pp 0 p_line)
                        list(GET pp 1 p_target)
                        list(APPEND found "${rel}|${p_target}")
                        list(APPEND found_where "${rel}:${p_line} -> :${p_target} (bare)")
                    endforeach()
                endif()
            endif()
            set(block_text "")
            set(block_pending "")
        endif()
    endforeach()
    # A comment run that reaches end-of-file never hits the else branch above.
    if(block_pending)
        set(padded " ${block_text} ")
        if(padded MATCHES "${NA_SPEC_NAME}" OR padded MATCHES "[^A-Za-z][Ss][Pp][Ee][Cc][^A-Za-z]")
            foreach(p IN LISTS block_pending)
                string(REPLACE "|" ";" pp "${p}")
                list(GET pp 0 p_line)
                list(GET pp 1 p_target)
                list(APPEND found "${rel}|${p_target}")
                list(APPEND found_where "${rel}:${p_line} -> :${p_target} (bare)")
            endforeach()
        endif()
    endif()
endforeach()

list(LENGTH found_where occurrence_count)
list(REMOVE_DUPLICATES found)

set(unregistered "${found}")
if(keys)
    list(REMOVE_ITEM unregistered ${keys})
endif()
set(stale "${keys}")
if(found)
    list(REMOVE_ITEM stale ${found})
endif()

if(unregistered OR stale)
    set(detail "")
    foreach(u IN LISTS unregistered)
        set(detail "${detail}\n  UNREGISTERED  ${u}   — cited in the tree, absent from citations.txt")
    endforeach()
    foreach(s IN LISTS stale)
        set(detail "${detail}\n  STALE         ${s}   — registered in citations.txt, no longer cited")
    endforeach()
    string(REPLACE ";" "\n  " where_pretty "${found_where}")
    message(FATAL_ERROR
        "spec-citations: the registered set and the tree disagree.${detail}\n\n"
        "Add or remove the entry in tests/spec-citations/citations.txt — an UNREGISTERED citation "
        "is not being checked by anything, which is the state this gate exists to end. Every "
        "citation found in the tree:\n  ${where_pretty}\n")
endif()

message(STATUS "spec-citations: ok   ${entry_count} registered citations, "
               "${occurrence_count} occurrences, ${scan_count} files scanned")

# ---------------------------------------------------------------------------
# Arm 1b — every registered citation still resolves to its anchor
# ---------------------------------------------------------------------------
na_check_anchors("${spec}" "${entries}" gate_fails)
if(gate_fails)
    string(REPLACE ";" "\n  " pretty "${gate_fails}")
    message(FATAL_ERROR
        "spec-citations: ${NA_SPEC_REL} no longer says what these citations claim it says:\n"
        "  ${pretty}\n\n"
        "Either the spec moved under the citation (find the anchor's new line and update the "
        "citing comment AND citations.txt), or the citation was always pointing at the wrong "
        "line. Do not 'fix' this by copying whatever text now sits at the cited line into the "
        "anchor — that repairs the gate instead of the citation.\n")
endif()

message(STATUS "spec-citations: ok   every citation resolves to its recorded anchor")

# ---------------------------------------------------------------------------
# Arm 2 — the positive control. One line inserted into a fixture copy of the spec must break every
# single-line citation. If it does not, arm 1b's silence above proves nothing.
#
# The insertion point is one line above the lowest registered target, so every citation shifts.
# Range citations are deliberately NOT required to fail: a range absorbs a shift smaller than its
# own width, which is a property of ranges rather than a hole in the gate — so the control asserts
# against the single-line entries, which cannot absorb anything.
# ---------------------------------------------------------------------------
set(min_target 0)
set(singles 0)
foreach(entry IN LISTS entries)
    string(REPLACE "|" ";" parts "${entry}")
    list(GET parts 1 t)
    na_parse_target("${t}" t_from t_to)
    if(min_target EQUAL 0 OR t_from LESS min_target)
        set(min_target "${t_from}")
    endif()
    if(t_from EQUAL t_to)
        math(EXPR singles "${singles} + 1")
    endif()
endforeach()

na_read_lines("${spec}" spec_lines)
set(fixture_text "")
set(n 0)
foreach(raw IN LISTS spec_lines)
    na_line("${raw}" line)
    math(EXPR n "${n} + 1")
    if(n EQUAL min_target)
        set(fixture_text "${fixture_text}A line inserted by check.cmake's positive control.\n")
    endif()
    set(fixture_text "${fixture_text}${line}\n")
endforeach()
set(fixture "${NA_WORK_DIR}/spec_shifted_control.md")
file(WRITE "${fixture}" "${fixture_text}")

na_check_anchors("${fixture}" "${entries}" control_fails)
list(LENGTH control_fails control_count)
if(control_count LESS singles)
    message(FATAL_ERROR
        "spec-citations: the positive control did not fire. A fixture spec with one line inserted "
        "at :${min_target} shifts every citation below it, so all ${singles} single-line citations "
        "must fail to find their anchors — only ${control_count} did. The anchor check is "
        "therefore not detecting line drift, and its clean result on the real spec above measures "
        "nothing.\nFixture: ${fixture}\n")
endif()

message(STATUS "spec-citations: ok   the shifted-spec control breaks ${control_count} citations "
               "(>= ${singles} single-line), so drift is detected")
