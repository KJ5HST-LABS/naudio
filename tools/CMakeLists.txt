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

if(NAUDIO_BUILD_APPS)
    add_executable(na_audio_daemon na_audio_daemon.cpp)
    target_link_libraries(na_audio_daemon PRIVATE naudio_net naudio_pa)
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
if(NAUDIO_BUILD_HAMLIB_BRIDGE)
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
                if(NAUDIO_HAMLIB_HAS_STREAM_CONV)
                    target_compile_definitions(na_hamlib_bridge PRIVATE NAUDIO_HAMLIB_HAS_STREAM_CONV)
                    message(STATUS "naudio: na_hamlib_bridge enabled (hamlib ${HAMLIB_VERSION}, "
                                   "conversion reporting available)")
                else()
                    # Not a warning: the bridge is fully functional here, it just cannot say
                    # whether libhamlib is converting for it. Worth one STATUS line so an operator
                    # who never sees a conversion report knows which of the two reasons applies.
                    message(STATUS "naudio: na_hamlib_bridge enabled (hamlib ${HAMLIB_VERSION}, "
                                   "pre-961093f2 — cannot report conversions)")
                endif()
            endif()
        endif()
    endif()
    unset(NAUDIO_HAMLIB_HELP)
endif()
