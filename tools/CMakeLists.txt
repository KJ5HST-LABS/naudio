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

# Built on every platform (issue #96 item 3). The NOT WIN32 gate that used to sit here excluded the
# daemon because it is a live-PortAudio hardware smoke Windows CI never runs — but that reasons from
# what CI can EXERCISE to what the build should PRODUCE, and the two are different questions. The
# same argument would have excluded it from macOS, where CI opens no device either. What the gate
# actually cost was the Windows operator: the daemon is the configure-and-run surface of the whole
# toolkit, so a Windows package without it is a dev kit, and #96's "no terminal" arc had nothing to
# register a service against. The hardware paths stay POSIX-smoked exactly as before — that is a
# statement about the smoke, not about the compiler.
if(NAUDIO_BUILD_APPS)
    add_executable(na_audio_daemon na_audio_daemon.cpp)
    target_link_libraries(na_audio_daemon PRIVATE naudio_net naudio_pa)
    # Ships in the binary packages (issue #93): the daemon is the operator-facing
    # surface, and an installer without it is a dev-kit-only installer.
    install(TARGETS na_audio_daemon
            RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT tools)
    # The man page is POSIX-only even though the tool is not — the Windows ZIP has no man tree and
    # no man(1) to read one (the same rule as na_wav_tap's and na_audio_source's pages below).
    if(NOT WIN32)
        install(FILES "${naudio_SOURCE_DIR}/docs/man/na_audio_daemon.1"
                DESTINATION "${CMAKE_INSTALL_MANDIR}/man1" COMPONENT tools)
    endif()

    # ------------------------------------------------------------------------
    # Service registration (issue #96 item 2) — run the daemon without a terminal.
    #
    # The unit files are GENERATED, never shipped verbatim: a launchd plist and a
    # systemd unit must both name an ABSOLUTE program path, and the only path that is
    # true is the one this build will install to. configure_file() ties them to
    # CMAKE_INSTALL_PREFIX, so the DEB/RPM units say /usr/bin and the .pkg's agent says
    # /usr/local/bin without either being hand-maintained. (A relocatable TGZ unpacked
    # somewhere else is the one case the generated path cannot follow — the man page
    # says so; the units exist for the packages.)
    #
    # Both are SESSION-scoped — a launchd Agent, a systemd --user unit — because audio
    # capture is a session privilege on both platforms (TCC on macOS, the per-user
    # PipeWire/PulseAudio socket on Linux). Rationale in full in each template.
    #
    # Windows has no arm here YET, and the reason is no longer that the daemon is missing —
    # it builds and ships there since item 3. What is missing is the registration: Windows
    # has no session-scoped service object equivalent to a launchd Agent or a systemd --user
    # unit. A real SCM service is the wrong shape for the same reason a LaunchDaemon was, one
    # layer down: services run in session 0, which has no audio endpoints, so it would install
    # cleanly and then capture nothing. The session-scoped analogue is a Task Scheduler logon
    # task (an XML definition, registered by an NSIS section, created disabled) — same three
    # properties this block already gives the other two platforms.
    # ------------------------------------------------------------------------
    set(NAUDIO_SERVICE_EXEC "${CMAKE_INSTALL_FULL_BINDIR}/na_audio_daemon")
    if(APPLE)
        set(NAUDIO_SERVICE_LOGDIR "${CMAKE_INSTALL_FULL_LOCALSTATEDIR}/log/naudio")
        set(NAUDIO_SERVICE_LOG "${NAUDIO_SERVICE_LOGDIR}/daemon.log")
        # Where the postinstall will find the plist inside the installed payload —
        # share/naudio/, because /Library/LaunchAgents is not under the .pkg's
        # /usr/local install prefix and the payload therefore cannot reach it.
        set(NAUDIO_PLIST_PAYLOAD_PATH
            "${CMAKE_INSTALL_FULL_DATADIR}/naudio/org.kj5hst.naudio.daemon.plist")
        configure_file(
            "${CMAKE_CURRENT_SOURCE_DIR}/../packaging/macos/org.kj5hst.naudio.daemon.plist.in"
            "${CMAKE_CURRENT_BINARY_DIR}/org.kj5hst.naudio.daemon.plist" @ONLY)
        configure_file("${CMAKE_CURRENT_SOURCE_DIR}/../packaging/macos/postinstall.in"
                       "${CMAKE_CURRENT_BINARY_DIR}/postinstall" @ONLY)
        # configure_file() writes 0644; productbuild runs this file as a script, and a
        # postinstall that is not executable is skipped by the installer rather than
        # failing it — the registration would silently not happen.
        file(CHMOD "${CMAKE_CURRENT_BINARY_DIR}/postinstall"
             PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
                         GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
        # Read at cpack time by cmake/CPackProjectConfig.cmake, productbuild branch only.
        set(NAUDIO_POSTFLIGHT_SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/postinstall" PARENT_SCOPE)
        install(FILES "${CMAKE_CURRENT_BINARY_DIR}/org.kj5hst.naudio.daemon.plist"
                DESTINATION "${CMAKE_INSTALL_DATADIR}/naudio" COMPONENT tools)
        set(NAUDIO_SERVICE_UNIT_FILE
            "${CMAKE_CURRENT_BINARY_DIR}/org.kj5hst.naudio.daemon.plist" PARENT_SCOPE)
    elseif(NOT WIN32)
        configure_file("${CMAKE_CURRENT_SOURCE_DIR}/../packaging/linux/naudio-daemon.service.in"
                       "${CMAKE_CURRENT_BINARY_DIR}/naudio-daemon.service" @ONLY)
        # lib/systemd/user is spelled out rather than taken from CMAKE_INSTALL_LIBDIR:
        # on a Debian-family host that variable becomes the multiarch
        # lib/x86_64-linux-gnu, and systemd does not search multiarch directories —
        # the unit would install where nothing would ever read it.
        install(FILES "${CMAKE_CURRENT_BINARY_DIR}/naudio-daemon.service"
                DESTINATION "lib/systemd/user" COMPONENT tools)
        set(NAUDIO_SERVICE_UNIT_FILE
            "${CMAKE_CURRENT_BINARY_DIR}/naudio-daemon.service" PARENT_SCOPE)
    endif()
    # Both read by the naudio_daemon_servicecheck arm, which asserts the unit names exactly
    # this path — the failure no build-tree test can see.
    set(NAUDIO_SERVICE_EXEC_PATH "${NAUDIO_SERVICE_EXEC}" PARENT_SCOPE)
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
    # Installed on every platform it builds on — Windows included, deliberately:
    # the remote end of an on-air check is frequently the operator's spare Windows
    # box, which is why the target itself is not WIN32-gated (issue #93).
    install(TARGETS na_wav_tap
            RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT tools)
    # The man page is POSIX-only even though the tool is not: the Windows ZIP
    # has no man tree and no man(1) to read one.
    if(NOT WIN32)
        install(FILES "${naudio_SOURCE_DIR}/docs/man/na_wav_tap.1"
                DESTINATION "${CMAKE_INSTALL_MANDIR}/man1" COMPONENT tools)
    endif()
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
            # Which link closure? A static-only prefix (build-streaming-hamlib.sh
            # --static, the release-packaging shape — issue #93) resolves
            # -lhamlib to libhamlib.a, whose PRIVATE closure (-ldl etc.) the
            # shared link line legitimately omits. Underlinking the archive
            # fails not just the final link but the capability PROBES below —
            # which report a plain FALSE on a link failure, so the bridge would
            # be skipped with a warning that blames the wrong thing. Decide by
            # what pkg-config actually resolved, never by a flag: a .a in the
            # list IS the static case. HAMLIB_STATIC_LDFLAGS (the raw -L/-l
            # flag list), not HAMLIB_STATIC_LINK_LIBRARIES — the latter is
            # EMPTY here (measured, CMake 4.1 FindPkgConfig against this
            # static-only prefix) while the flag list carries the closure.
            if(HAMLIB_LINK_LIBRARIES MATCHES "\\.a(;|$)")
                set(NAUDIO_HAMLIB_LIBS ${HAMLIB_STATIC_LDFLAGS})
                # Read by the fault-arm registration in the top-level CMakeLists
                # (after add_subdirectory(tools)): a preload shim cannot
                # interpose calls into a statically linked hamlib.
                set(NAUDIO_HAMLIB_STATIC_ONLY TRUE PARENT_SCOPE)
            else()
                set(NAUDIO_HAMLIB_LIBS ${HAMLIB_LINK_LIBRARIES})
                set(NAUDIO_HAMLIB_STATIC_ONLY FALSE PARENT_SCOPE)
            endif()
            include(CheckSymbolExists)
            set(CMAKE_REQUIRED_INCLUDES ${HAMLIB_INCLUDE_DIRS})
            set(CMAKE_REQUIRED_LIBRARIES ${NAUDIO_HAMLIB_LIBS})
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
            # Hamlib PR #2172 then turned rig_stream_config.require_native from an all-or-nothing
            # boolean into a per-stage bitmask of the same RIG_STREAM_CONV_* constants the
            # conversion report speaks — which is what lets the bridge demand a native sample
            # rate while still accepting the frontend's format conversion (the request naudio
            # made on that PR). RIG_STREAM_CONV_ALL is an object-like macro added by the same
            # commit as the mask semantics, so its presence is exactly "require_native takes
            # stages", and check_symbol_exists can see a macro.
            #
            # Verified to discriminate against two real prefixes: upstream master e4c8d09
            # reports FALSE, the PR #2172 head a3fc759 reports TRUE.
            check_symbol_exists(RIG_STREAM_CONV_ALL "hamlib/rig.h"
                                NAUDIO_HAMLIB_HAS_REQUIRE_NATIVE_MASK)
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
                                      PRIVATE naudio ${NAUDIO_HAMLIB_LIBS} Threads::Threads)
                # Installed for whoever BUILDS it — but deliberately absent from
                # the binary release packages since 1.0.0rc2: the release shape
                # does not enable this option, so no release artifact embeds or
                # references any hamlib (the release gates assert that absence).
                # It returns to the packages, linked against the system's real
                # libhamlib, once a released Hamlib ships the streaming API.
                install(TARGETS na_hamlib_bridge
                        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT tools)
                # Installed only when the target is: a man page for a tool the
                # package does not carry would document a phantom.
                install(FILES "${naudio_SOURCE_DIR}/docs/man/na_hamlib_bridge.1"
                        DESTINATION "${CMAKE_INSTALL_MANDIR}/man1" COMPONENT tools)
                if(NAUDIO_HAMLIB_HAS_STREAM_CHANNEL_LIST)
                    target_compile_definitions(na_hamlib_bridge
                                               PRIVATE NAUDIO_HAMLIB_HAS_STREAM_CHANNEL_LIST)
                endif()
                if(NAUDIO_HAMLIB_HAS_REQUIRE_NATIVE_MASK)
                    target_compile_definitions(na_hamlib_bridge
                                               PRIVATE NAUDIO_HAMLIB_HAS_REQUIRE_NATIVE_MASK)
                endif()
                if(NAUDIO_HAMLIB_HAS_STREAM_CONV)
                    target_compile_definitions(na_hamlib_bridge PRIVATE NAUDIO_HAMLIB_HAS_STREAM_CONV)
                    if(NAUDIO_HAMLIB_HAS_REQUIRE_NATIVE_MASK)
                        message(STATUS "naudio: na_hamlib_bridge enabled (hamlib ${HAMLIB_VERSION}, "
                                       "conversion reporting available, native sample rate enforced "
                                       "at open)")
                    else()
                        # Reported, not enforced: this libhamlib predates PR #2172's per-stage
                        # require_native, whose all-or-nothing predecessor would refuse the format
                        # conversion the bridge deliberately accepts. Still a STATUS, not a
                        # WARNING — the bridge behaves exactly as it did before that PR existed.
                        message(STATUS "naudio: na_hamlib_bridge enabled (hamlib ${HAMLIB_VERSION}, "
                                       "conversion reporting available; a resample would be "
                                       "reported but not refused — pre-PR-#2172 require_native)")
                    endif()
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
