# ----------------------------------------------------------------------------
# tools/ — na_audio_daemon: the hardware-smoke driver. Deliberately OUTSIDE
# naudio_core/naudio.
#
# na_audio_daemon is the HARDWARE-SMOKE driver. It runs the real
# AudioStreamServer / AudioStreamClient over a real
# PortAudio device — the path ctest (FakeBackend only) cannot reach. It links
# the C++ classes directly: naudio_net (transport/server/client) + naudio_pa
# (PortAudioBackend + core) — NOT the public hidden-visibility shared `naudio`
# (which exports only the na_* C ABI).
# ----------------------------------------------------------------------------

# NOT WIN32 is the gate that used to sit on this whole directory in the top-level CMakeLists: the
# daemon is a live-PortAudio hardware smoke that Windows CI never runs. It stays excluded there.
if(NAUDIO_BUILD_APPS AND NOT WIN32)
    add_executable(na_audio_daemon na_audio_daemon.cpp)
    target_link_libraries(na_audio_daemon PRIVATE naudio_net naudio_pa)
endif()

# ----------------------------------------------------------------------------
# na_wav_tap: record what a naudio CLIENT receives, as a WAV a decoder can read.
#
# The evidence tool for issue #12 item 1 ("RX audio ... is intelligible"). Judged by ear that item
# leaves no artifact and cannot be re-checked by anyone who was not present; for a digital mode it
# becomes objective — audio carried end to end through naudio either still decodes to real
# callsigns or it does not. See docs/on-air-verification.md.
#
# Unlike na_audio_daemon above, this links the PUBLIC hidden-visibility `naudio` (the na_* C ABI)
# and nothing else, deliberately: the file it writes is then what a third-party consumer of the
# shipped contract receives, not what an internal buffer held. Pure C, same posture as
# na_hamlib_bridge.
#
# POSIX-only (clock_gettime / usleep / sleep), so gated like the examples rather than by
# NAUDIO_BUILD_APPS alone.
# ----------------------------------------------------------------------------
# Builds on Windows too, unlike everything else in here — see the note in the top-level
# CMakeLists. It opens no device, so there is nothing platform-specific to exclude, and the
# remote end of an on-air check is frequently the operator's spare Windows box.
if(NAUDIO_BUILD_APPS)
    add_executable(na_wav_tap na_wav_tap.c)
    target_link_libraries(na_wav_tap PRIVATE naudio)
endif()

# ----------------------------------------------------------------------------
# na_hamlib_bridge: optional Hamlib-streaming (PR #2116) <-> naudio bridge.
#
# Re-originates a rig_stream_* audio stream onto naudio's FEC-native wire for the
# lossy/WAN hop. Pure C, linking the public `naudio` C ABI + libhamlib. OFF by
# default and only built when libhamlib is present AND actually exposes the
# rig_stream_* streaming API (Hamlib 5.0+ / PR #2116) — so the clean, self-
# contained build is untouched and a stock libhamlib (no streaming) cannot break
# the build. Point pkg-config at a streaming-capable libhamlib to enable it:
#   cmake -S . -B build -DNAUDIO_BUILD_HAMLIB_BRIDGE=ON
#   (with PKG_CONFIG_PATH=<hamlib-prefix>/lib/pkgconfig)
#
# No released Hamlib has rig_stream_*, so that prefix has to be built. Build one
# with tools/build-streaming-hamlib.sh; docs/hamlib-streaming-bridge.md covers
# the manual path and why a skipped target looks like a broken flag.
# ----------------------------------------------------------------------------
option(NAUDIO_BUILD_HAMLIB_BRIDGE
       "Build the Hamlib streaming <-> naudio bridge (needs libhamlib with rig_stream_*)" OFF)
if(NAUDIO_BUILD_HAMLIB_BRIDGE AND NOT WIN32)
    # Every skip below is a WARNING, not an error: the flag is opt-in and the rest
    # of the build stays green. That makes the skip easy to scroll past, so each
    # one says where to get the missing dependency.
    set(NAUDIO_HAMLIB_HELP "\n  Build one:  tools/build-streaming-hamlib.sh"
                           "\n  Details:    docs/hamlib-streaming-bridge.md")
    find_package(PkgConfig)
    if(NOT PkgConfig_FOUND)
        message(WARNING "NAUDIO_BUILD_HAMLIB_BRIDGE: pkg-config not found; skipping na_hamlib_bridge"
                        ${NAUDIO_HAMLIB_HELP})
    else()
        pkg_check_modules(HAMLIB hamlib)
        if(NOT HAMLIB_FOUND)
            message(WARNING "NAUDIO_BUILD_HAMLIB_BRIDGE: libhamlib not found via pkg-config; "
                            "skipping na_hamlib_bridge. Set PKG_CONFIG_PATH=<prefix>/lib/pkgconfig "
                            "on the cmake configure line." ${NAUDIO_HAMLIB_HELP})
        else()
            include(CheckSymbolExists)
            set(CMAKE_REQUIRED_INCLUDES ${HAMLIB_INCLUDE_DIRS})
            set(CMAKE_REQUIRED_LIBRARIES ${HAMLIB_LINK_LIBRARIES})
            check_symbol_exists(rig_stream_open "hamlib/rig.h" NAUDIO_HAMLIB_HAS_STREAM)
            # Hamlib PR #2116 commit 961093f2 moved sample-format/rate/channel conversion out of
            # the backends and into the streaming core, and added rig_stream_get_conversions() to
            # report which stages are running. The gate above is a PRESENCE check on the streaming
            # API as a whole, so a prefix built before that commit satisfies it and still lacks
            # this call. Detect it separately rather than assuming the two arrived together.
            check_symbol_exists(rig_stream_get_conversions "hamlib/rig.h"
                                NAUDIO_HAMLIB_HAS_STREAM_CONV)
            # Commit b538567b then replaced rig_stream_caps' channels_min/channels_max RANGE with
            # an exact 0-terminated channels[] LIST (openable counts need not be contiguous). That
            # is a struct-field change, so check_symbol_exists cannot see it — a prefix on either
            # side of the commit exports exactly the same symbols, and the mismatch surfaces only
            # as a compile error in na_hamlib_bridge.c. naudio tracks the PR head deliberately
            # rather than pinning a commit (#81, docs/hamlib-streaming-bridge.md), so BOTH shapes
            # remain reachable from a locally built prefix and neither may be assumed.
            #
            # Verified to discriminate against three real prefixes: pre-961093f2 and 961093f2 both
            # report FALSE, the b538567b head reports TRUE.
            include(CheckStructHasMember)
            check_struct_has_member("struct rig_stream_caps" channels "hamlib/rig.h"
                                    NAUDIO_HAMLIB_HAS_STREAM_CHANNEL_LIST LANGUAGE C)
            unset(CMAKE_REQUIRED_INCLUDES)
            unset(CMAKE_REQUIRED_LIBRARIES)
            if(NOT NAUDIO_HAMLIB_HAS_STREAM)
                message(WARNING "NAUDIO_BUILD_HAMLIB_BRIDGE: libhamlib ${HAMLIB_VERSION} lacks "
                                "rig_stream_* (needs Hamlib 5.0+/PR #2116); skipping na_hamlib_bridge"
                                ${NAUDIO_HAMLIB_HELP})
            else()
                add_executable(na_hamlib_bridge na_hamlib_bridge.c)
                target_include_directories(na_hamlib_bridge PRIVATE ${HAMLIB_INCLUDE_DIRS})
                target_link_directories(na_hamlib_bridge PRIVATE ${HAMLIB_LIBRARY_DIRS})
                target_link_libraries(na_hamlib_bridge
                                      PRIVATE naudio ${HAMLIB_LINK_LIBRARIES} Threads::Threads)
                if(NAUDIO_HAMLIB_HAS_STREAM_CHANNEL_LIST)
                    target_compile_definitions(na_hamlib_bridge
                                               PRIVATE NAUDIO_HAMLIB_HAS_STREAM_CHANNEL_LIST)
                endif()
                if(NAUDIO_HAMLIB_HAS_STREAM_CONV)
                    target_compile_definitions(na_hamlib_bridge PRIVATE NAUDIO_HAMLIB_HAS_STREAM_CONV)
                    message(STATUS "naudio: na_hamlib_bridge enabled (hamlib ${HAMLIB_VERSION}, "
                                   "conversion reporting available)")
                else()
                    # Not a warning: the bridge is fully functional here, it just cannot say
                    # whether libhamlib is converting for it. Worth one STATUS line so an operator
                    # who never sees a conversion report knows which of the two reasons applies.
                    #
                    # It also names the -c 2 floor, because that is the consequence an operator
                    # actually hits: pre-961093f2 there is no channels field on the \stream_open
                    # wire, so stereo over netrigctl (-m 2) is mis-framed rather than refused
                    # (measured: ~1 gap per RX read at 41% of nominal). Said here rather than only
                    # at runtime — this is the moment the choice of prefix is still in hand.
                    message(STATUS "naudio: na_hamlib_bridge enabled (hamlib ${HAMLIB_VERSION}, "
                                   "pre-961093f2 — cannot report conversions, and -c 2 over "
                                   "netrigctl is mis-framed; use -c 1 or build against a newer "
                                   "libhamlib)")
                endif()
            endif()
        endif()
    endif()
    unset(NAUDIO_HAMLIB_HELP)
endif()
