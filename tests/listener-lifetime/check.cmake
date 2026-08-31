# SPDX-License-Identifier: LGPL-2.1-or-later
#
# naudio — the listener-lifetime enumeration gate. Driven by the `naudio_listener_lifetime` ctest
# arm; see the add_test() comment in the root CMakeLists.txt for why it exists (issue #97).
#
# Run as: cmake -D NA_SOURCE_DIR=... -P check.cmake
#
# WHAT IT CHECKS, EXACTLY. Every addStreamListener() CALL in the tree must appear in sites.txt
# with a stated lifetime mechanism, and every registered site must still exist. It does NOT read
# declaration order — that needs a C++ front end, and a hand-rolled one is how this gate would
# start lying (a first attempt at one silently reported tools/na_audio_daemon.cpp clean while that
# file held a live instance of the very defect, because an unbalanced brace inside a string
# literal made the whole file parse as one unterminated block). So this gate does the part that
# can be done soundly with line matching: it makes a NEW call site impossible to add silently.
# Recurrence here has always been a new site — 14 arms in test_client_e2e.cpp, one in
# test_server.cpp, one in na_audio_daemon.cpp — never an old one flipped.
#
# TWO ARMS, and the second is not decoration, for the same reason the spec-citation gate next door
# gives: this gate passes by finding nothing unregistered, which is exactly what a regex that
# matches nothing also produces. So arm 2 runs the SAME extractor over a fixture carrying two
# known call sites and requires both to be seen. Only then does arm 1's silence mean anything.

foreach(var NA_SOURCE_DIR)
    if(NOT DEFINED ${var} OR "${${var}}" STREQUAL "")
        message(FATAL_ERROR "listener-lifetime: ${var} was not passed to the script")
    endif()
endforeach()

set(manifest "${NA_SOURCE_DIR}/tests/listener-lifetime/sites.txt")
set(fixture "${NA_SOURCE_DIR}/tests/listener-lifetime/fixture.cc.in")
foreach(f "${manifest}" "${fixture}")
    if(NOT EXISTS "${f}")
        message(FATAL_ERROR "listener-lifetime: ${f} is missing")
    endif()
endforeach()

# A CALL, not a mention. The leading `<ident>.` / `<ident>->` is what separates
# `client.addStreamListener(&listener)` from the header's prose ("handed back with
# removeStreamListener() below") and from this comment. The argument list runs to the last `)`
# before the statement's `;`, which keeps `server->glue.get()` whole.
set(NA_CALL_RE "[A-Za-z_][A-Za-z0-9_]*(\\.|->)addStreamListener[ \t]*\\([^;]*\\)")

# Extract every call in `path` and append "<rel>|<call>" to the list named by `out_var`.
function(na_extract path rel out_var)
    set(found "${${out_var}}")
    # ENCODING UTF-8 is load-bearing, not decoration. file(STRINGS) defaults to extracting ASCII
    # strings and SPLITS a line at the first non-ASCII byte — and this tree writes em dashes in
    # nearly every comment. Without it a line is silently delivered as two fragments, which is a
    # harness that reports a different answer than the file contains. Measured: the manifest's own
    # header comment arrived with its `# naudio — ` prefix gone and failed the format check.
    file(STRINGS "${path}" hits ENCODING UTF-8 REGEX "addStreamListener")
    foreach(line IN LISTS hits)
        # A whole-line `//` comment is not a call. This is the only comment form handled, and
        # deliberately so: a call inside a block comment or after code on the same line would
        # still be counted, which errs toward MORE manifest entries rather than fewer.
        if("${line}" MATCHES "^[ \t]*//")
            continue()
        endif()
        string(REGEX MATCH "${NA_CALL_RE}" call "${line}")
        if(NOT "${call}" STREQUAL "")
            list(APPEND found "${rel}|${call}")
        endif()
    endforeach()
    set(${out_var} "${found}" PARENT_SCOPE)
endfunction()

# ---------------------------------------------------------------------------
# ARM 2 FIRST — the extractor must be shown to SEE before its silence is evidence.
# ---------------------------------------------------------------------------
set(fixture_found "")
na_extract("${fixture}" "fixture" fixture_found)
set(fixture_want "fixture|owner.addStreamListener(&aListener)"
                 "fixture|handle->addStreamListener(handle->glue.get())")
foreach(want IN LISTS fixture_want)
    list(FIND fixture_found "${want}" idx)
    if(idx LESS 0)
        message(FATAL_ERROR
            "listener-lifetime: the extractor did NOT find `${want}` in the fixture.\n"
            "  This gate reports a clean tree by finding nothing unregistered, so an extractor\n"
            "  that matches nothing would pass forever. It does not match. Fix NA_CALL_RE in\n"
            "  tests/listener-lifetime/check.cmake — do not touch the fixture to make this pass.\n"
            "  saw: ${fixture_found}")
    endif()
endforeach()
list(LENGTH fixture_found n_fixture)
if(NOT n_fixture EQUAL 2)
    message(FATAL_ERROR
        "listener-lifetime: the fixture holds exactly 2 calls plus look-alikes that must NOT "
        "match (prose naming the function, and a commented-out call); the extractor returned "
        "${n_fixture} matches, so it is over- or under-matching.\n  saw: ${fixture_found}")
endif()

# ---------------------------------------------------------------------------
# ARM 1 — the tree against the manifest.
# ---------------------------------------------------------------------------
set(found "")
foreach(dir src tests tools examples include)
    file(GLOB_RECURSE srcs RELATIVE "${NA_SOURCE_DIR}"
         "${NA_SOURCE_DIR}/${dir}/*.c" "${NA_SOURCE_DIR}/${dir}/*.cc"
         "${NA_SOURCE_DIR}/${dir}/*.cpp" "${NA_SOURCE_DIR}/${dir}/*.h"
         "${NA_SOURCE_DIR}/${dir}/*.hpp")
    foreach(rel IN LISTS srcs)
        na_extract("${NA_SOURCE_DIR}/${rel}" "${rel}" found)
    endforeach()
endforeach()

# Fold to "<rel>|<call>|<count>".
set(keys "")
foreach(f IN LISTS found)
    list(APPEND keys "${f}")
endforeach()
list(REMOVE_DUPLICATES keys)
set(actual "")
foreach(k IN LISTS keys)
    set(n 0)
    foreach(f IN LISTS found)
        if("${f}" STREQUAL "${k}")
            math(EXPR n "${n} + 1")
        endif()
    endforeach()
    list(APPEND actual "${k}|${n}")
endforeach()

# The manifest, minus comments and blanks, minus the mechanism column.
file(STRINGS "${manifest}" manifest_lines ENCODING UTF-8)
set(registered "")
set(mechanisms "")
foreach(line IN LISTS manifest_lines)
    string(STRIP "${line}" line)
    if("${line}" STREQUAL "" OR "${line}" MATCHES "^#")
        continue()
    endif()
    if(NOT "${line}" MATCHES "^([^|]+)\\|(.+)\\|([0-9]+)\\|(declared-before|handed-back|member-order)$")
        message(FATAL_ERROR
            "listener-lifetime: sites.txt line is not <path>|<call>|<count>|<mechanism>, or names "
            "a mechanism that is not one of declared-before / handed-back / member-order:\n  ${line}")
    endif()
    list(APPEND registered "${CMAKE_MATCH_1}|${CMAKE_MATCH_2}|${CMAKE_MATCH_3}")
    list(APPEND mechanisms "${CMAKE_MATCH_4}")
endforeach()

set(unregistered "")
foreach(a IN LISTS actual)
    list(FIND registered "${a}" idx)
    if(idx LESS 0)
        list(APPEND unregistered "${a}")
    endif()
endforeach()
set(stale "")
foreach(r IN LISTS registered)
    list(FIND actual "${r}" idx)
    if(idx LESS 0)
        list(APPEND stale "${r}")
    endif()
endforeach()

if(NOT "${unregistered}" STREQUAL "" OR NOT "${stale}" STREQUAL "")
    set(msg "listener-lifetime: tests/listener-lifetime/sites.txt no longer describes the tree.\n")
    foreach(u IN LISTS unregistered)
        string(APPEND msg "  UNREGISTERED (or the count changed): ${u}\n")
    endforeach()
    foreach(s IN LISTS stale)
        string(APPEND msg "  REGISTERED BUT NOT FOUND: ${s}\n")
    endforeach()
    string(APPEND msg
        "\n"
        "  addStreamListener() BORROWS the pointer. Every notify* snapshots the roster and\n"
        "  captures the raw pointers by value into a task the dispatcher runs LATER; stop() and\n"
        "  disconnect() only POST the final callback, and the drain + join happen in the owner's\n"
        "  destructor. So a listener destroyed after the last stop() but before the owner is read\n"
        "  through by the drain. That is issue #97 (and #28 before it).\n"
        "\n"
        "  Add or update the line, choosing the mechanism this site actually uses:\n"
        "    declared-before  listener declared before the owner in the same scope\n"
        "    handed-back      removeStreamListener() called before the listener dies (it fences)\n"
        "    member-order     both are members of one struct, owner destroyed first\n")
    message(FATAL_ERROR "${msg}")
endif()

list(LENGTH actual n_sites)
list(LENGTH found n_calls)
message(STATUS "listener-lifetime: ${n_calls} calls across ${n_sites} registered sites; "
               "fixture extractor verified")
