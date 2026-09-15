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
    # ------------------------------------------------------------------------
    # The control page (issue #96 item 4), embedded rather than installed.
    #
    # A page read from disk at runtime is a path to locate on three platforms, a file a package
    # can fail to install, and — the one that decides it — a file anything with write access to
    # the install tree could replace with script the browser would then run as same-origin. So
    # both assets are compiled in. The generated file is a single translation unit holding two
    # raw string literals.
    #
    # The delimiter check below is not ceremony: a `)NAUDIO_ASSET"` appearing in the HTML would
    # end the literal early and produce a compile error pointing at the GENERATED file, which is
    # a bad place to learn what went wrong. It fails here instead, naming the source asset.
    foreach(_asset control_page.html control_page.js)
        if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_asset}")
            message(FATAL_ERROR "naudio: missing control-page asset ${_asset}")
        endif()
        # file(READ) creates no reconfigure dependency the way configure_file's INPUT does, so
        # editing the page would otherwise leave a stale binary behind until something else
        # forced a re-run of CMake.
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                     "${CMAKE_CURRENT_SOURCE_DIR}/${_asset}")
    endforeach()
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/control_page.html" NAUDIO_CONTROL_PAGE_HTML)
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/control_page.js" NAUDIO_CONTROL_PAGE_JS)
    foreach(_content "${NAUDIO_CONTROL_PAGE_HTML}" "${NAUDIO_CONTROL_PAGE_JS}")
        string(FIND "${_content}" ")NAUDIO_ASSET\"" _clash)
        if(NOT _clash EQUAL -1)
            message(FATAL_ERROR
                    "naudio: a control-page asset contains the raw-string delimiter "
                    ")NAUDIO_ASSET\" — rename the delimiter in control_page.cpp.in")
        endif()
    endforeach()
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/control_page.cpp.in"
                   "${CMAKE_CURRENT_BINARY_DIR}/control_page.cpp" @ONLY)

    # The icon (issue #103) is one file per platform container, all generated from
    # packaging/icons/naudio.png by packaging/icons/regenerate.py and checked in — see that
    # script for why the build does not make them. On Windows it is compiled INTO the daemon as
    # its icon resource: Explorer, the taskbar, the Start-menu shortcut (which shows its
    # target's icon) and Add/Remove Programs (CPACK_NSIS_INSTALLED_ICON_NAME names this
    # executable) all read it from there. The .rc is generated so it names this checkout's
    # .ico by absolute path, which is the one path both rc.exe and windres resolve from any
    # working directory. Only the daemon carries it — the other tools are console programs
    # with no launcher — and the release gate uses one of those as the zero-icon control.
    set(NAUDIO_ICON_DIR "${naudio_SOURCE_DIR}/packaging/icons")
    set(_daemon_icon_sources "")
    if(WIN32)
        set(NAUDIO_ICO_PATH "${NAUDIO_ICON_DIR}/naudio.ico")
        if(NOT EXISTS "${NAUDIO_ICO_PATH}")
            message(FATAL_ERROR "naudio: missing icon ${NAUDIO_ICO_PATH}")
        endif()
        configure_file("${CMAKE_CURRENT_SOURCE_DIR}/../packaging/windows/na_audio_daemon.rc.in"
                       "${CMAKE_CURRENT_BINARY_DIR}/na_audio_daemon.rc" @ONLY)
        set(_daemon_icon_sources "${CMAKE_CURRENT_BINARY_DIR}/na_audio_daemon.rc")
    endif()

    add_executable(na_audio_daemon na_audio_daemon.cpp
                                   "${CMAKE_CURRENT_BINARY_DIR}/control_page.cpp"
                                   ${_daemon_icon_sources})
    target_link_libraries(na_audio_daemon PRIVATE naudio_net naudio_pa)
    # The control page names the version it is part of; there is no other way for a tool to know
    # it, since it links the internal static libraries rather than the versioned shared one.
    target_compile_definitions(na_audio_daemon
                               PRIVATE NAUDIO_TOOL_VERSION="${PROJECT_VERSION}")
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
    # Windows now has its arm too: a Task Scheduler LOGON TASK. A real SCM service is the wrong
    # shape for the same reason a LaunchDaemon was, one layer down — services run in session 0,
    # which has no audio endpoints, so one would install cleanly and then capture nothing. A
    # logon task runs in the interactive session, where the devices are, and it takes the same
    # three properties: an absolute program path, generated rather than hand-written; created
    # DISABLED so installing starts nothing (the systemd unit is likewise not enabled); removed
    # on uninstall. The launchd agent is the odd one out since issue #102: macOS has a system
    # switch for background items (Login Items) and it disagrees with a Disabled key, so the
    # agent ships without one and is on by default — inert because it runs control mode with
    # autostart off, which the servicecheck arm proves by running the unit's own arguments.
    #
    # One asymmetry Windows forces and the other two do not: its install prefix is chosen by the
    # operator on the installer's directory page, so the task XML is rendered TWICE — once here
    # against this build's install path (what the servicecheck arm asserts on), and again by the
    # installer against $INSTDIR (what actually gets registered). See
    # packaging/windows/register-task.ps1.
    #
    # All three units run `--mode control` since issue #96 item 4. That is a strict superset of
    # what they ran before: control mode serves the local page and captures NOTHING until asked,
    # so enabling the service gives the operator something to click rather than an open
    # microphone. Capture-at-login is `autostart = true`, which the page writes from a checkbox.
    # ------------------------------------------------------------------------
    set(NAUDIO_SERVICE_EXEC "${CMAKE_INSTALL_FULL_BINDIR}/na_audio_daemon")
    if(WIN32)
        # The task's <Command> must name a file that exists; without the suffix it names nothing.
        set(NAUDIO_SERVICE_EXEC "${NAUDIO_SERVICE_EXEC}.exe")
    endif()
    if(APPLE)
        # On macOS the agent runs the daemon FROM INSIDE Network Audio Service.app (issue #105),
        # not from bin/: TCC names a process after its bundle, and after nothing else — the
        # Microphone prompt and the Privacy & Security row for an agent run from
        # /usr/local/bin read "na_audio_daemon" with a generic icon, and an Info.plist embedded
        # in that executable changed neither (measured 2026-09-15, macOS 27). Run as the
        # bundle's own executable, the same daemon is "Network Audio Service" with its icon in
        # both places, and the usage sentence in the bundle's Info.plist is the one the prompt
        # shows. The name and the path are owned HERE, and the plist, the postinstall and the
        # bundle's Info.plist all take them from here, because the three must be one string:
        # the Finder shows the on-disk name and ignores a CFBundleDisplayName that differs
        # from it (measured 2026-09-14), and Login Items shows the display name.
        set(NAUDIO_APP_NAME "Network Audio Service")
        set(NAUDIO_APP_DIR "/Applications/${NAUDIO_APP_NAME}.app")
        set(NAUDIO_SERVICE_EXEC "${NAUDIO_APP_DIR}/Contents/MacOS/na_audio_daemon")
        set(NAUDIO_SERVICE_LOGDIR "${CMAKE_INSTALL_FULL_LOCALSTATEDIR}/log/naudio")
        set(NAUDIO_SERVICE_LOG "${NAUDIO_SERVICE_LOGDIR}/daemon.log")
        # Where the postinstall will find the plist inside the installed payload —
        # share/naudio/, because /Library/LaunchAgents is not under the .pkg's
        # /usr/local install prefix and the payload therefore cannot reach it.
        set(NAUDIO_PLIST_PAYLOAD_PATH
            "${CMAKE_INSTALL_FULL_DATADIR}/naudio/org.kj5hst.naudio.daemon.plist")
        # Where the postinstall will find the control app's TWO PIECES inside the payload — and,
        # when the payload is signed, its signature directory naudio-control.codesig/, which
        # cmake/CodesignPayload.cmake writes beside them at cpack time (issue #102). Set HERE
        # rather than beside their configure_file below, because postinstall.in is configured a
        # few lines down and a variable set after it would expand to nothing.
        set(NAUDIO_APP_PAYLOAD_DIR "${CMAKE_INSTALL_FULL_DATADIR}/naudio")
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

        # "Network Audio Service.app" (issue #96 item 4) — the Applications entry, because macOS
        # has no .desktop equivalent and the issue's acceptance is that the page opens from an
        # INSTALLED shortcut. Since issue #105 its executable is THE DAEMON ITSELF, a second copy
        # of na_audio_daemon: the launchd agent runs that copy (NAUDIO_SERVICE_EXEC above), so
        # the process TCC sees is the bundle, and a double-click runs the same copy with no
        # arguments, which the daemon takes as "open the control page" (launchedFromBundle in
        # na_audio_daemon.cpp) — what the three-line launcher script it replaces used to pass.
        # Like the plist it rides inside the payload and is placed by the postinstall, because
        # /Applications is not under the /usr/local install prefix either. Named after the
        # service rather than the page because on macOS this bundle IS the service: what Login
        # Items shows the agent as (issue #102) and what the Microphone prompt names (#105);
        # the Linux and Windows shortcuts below keep the page's name, having no such row.
        #
        # THE PAYLOAD CARRIES FLAT FILES, NOT A BUNDLE, and that is the whole point. Shipping
        # the assembled `.app` inside the payload made `pkgbuild` auto-detect it and write
        # <relocate>, <strict-identifier>, <upgrade-bundle> and <bundle-version> entries into
        # PackageInfo — machinery that lets macOS Installer decide for itself where, or whether,
        # to place the bundle by consulting Spotlight for the bundle identifier. It did not place
        # it on a CI runner, silently, while the payload listing showed the files present.
        # Ordinary files cannot be auto-detected as a bundle, so the postinstall's `install`
        # calls are the only thing that decides where this lands.
        #
        # The same reasoning is why a SIGNED release carries the bundle's seal as loose files
        # too (share/naudio/app.codesig/, written at cpack time by cmake/CodesignPayload.cmake,
        # which explains why the bundle must be signed at all): a directory of data files is not
        # a bundle either, and the postinstall installs them into Contents/_CodeSignature/
        # beside the other pieces. The executable's own signature travels INSIDE it, as it does
        # for any Mach-O: CodesignPayload.cmake signs the bundle and writes the bundle-signed
        # executable back over the staged copy.
        configure_file("${CMAKE_CURRENT_SOURCE_DIR}/../packaging/macos/app/Info.plist.in"
                       "${CMAKE_CURRENT_BINARY_DIR}/naudio-control.Info.plist" @ONLY)
        install(FILES "${CMAKE_CURRENT_BINARY_DIR}/naudio-control.Info.plist"
                DESTINATION "${CMAKE_INSTALL_DATADIR}/naudio" COMPONENT tools)
        # Read by the naudio_daemon_servicecheck arm: the agent's AssociatedBundleIdentifiers
        # must be this file's CFBundleIdentifier (issue #102), the agent's program must be the
        # file's CFBundleExecutable inside the bundle named after its CFBundleName (issue
        # #105) — asserted as relations between the two generated files rather than as
        # literals either could drift from.
        set(NAUDIO_APP_INFO_PLIST
            "${CMAKE_CURRENT_BINARY_DIR}/naudio-control.Info.plist" PARENT_SCOPE)
        # The bundle's executable: the daemon, installed a SECOND time beside the plist. A
        # target install, not a PROGRAMS copy of the build-tree file, so CPack strips it and
        # rewrites its install RPATH exactly as it does the bin/ copy — the two must be the
        # same bytes before signing. A symlink would not do: the kernel resolves it, so the
        # process would be /usr/local/bin's again and TCC would name the file.
        install(TARGETS na_audio_daemon
                RUNTIME DESTINATION "${CMAKE_INSTALL_DATADIR}/naudio" COMPONENT tools)
        # The THIRD piece (issue #103): the bundle's icon, under the name Info.plist.in's
        # CFBundleIconFile gives it. The postinstall places it in Contents/Resources; the seal
        # covers Resources, so cmake/CodesignPayload.cmake assembles it into the bundle it
        # signs, and refuses a staging tree where it is missing (a sealed bundle without it
        # would still verify — the notary never sees it — but it would have no icon).
        if(NOT EXISTS "${NAUDIO_ICON_DIR}/naudio.icns")
            message(FATAL_ERROR "naudio: missing icon ${NAUDIO_ICON_DIR}/naudio.icns")
        endif()
        install(FILES "${NAUDIO_ICON_DIR}/naudio.icns"
                DESTINATION "${CMAKE_INSTALL_DATADIR}/naudio" COMPONENT tools)
    elseif(WIN32)
        # For the servicecheck arm: this build's install path, rendered here. The file the
        # installer actually registers is rendered from the same template against $INSTDIR.
        configure_file("${CMAKE_CURRENT_SOURCE_DIR}/../packaging/windows/naudio-daemon-task.xml.in"
                       "${CMAKE_CURRENT_BINARY_DIR}/naudio-daemon-task.xml" @ONLY)
        # The TEMPLATE ships, not the rendering: the installer needs the unsubstituted token.
        install(FILES
                "${CMAKE_CURRENT_SOURCE_DIR}/../packaging/windows/naudio-daemon-task.xml.in"
                "${CMAKE_CURRENT_SOURCE_DIR}/../packaging/windows/register-task.ps1"
                DESTINATION "${CMAKE_INSTALL_DATADIR}/naudio" COMPONENT tools)
        set(NAUDIO_SERVICE_UNIT_FILE
            "${CMAKE_CURRENT_BINARY_DIR}/naudio-daemon-task.xml" PARENT_SCOPE)
    else()
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

        # The desktop entry (issue #96 item 4). Unlike the systemd unit this needs no special
        # directory reasoning: share/applications IS under the /usr prefix the DEB and RPM use,
        # so the payload places it directly and no postinst is involved.
        configure_file("${CMAKE_CURRENT_SOURCE_DIR}/../packaging/linux/naudio-control.desktop.in"
                       "${CMAKE_CURRENT_BINARY_DIR}/naudio-control.desktop" @ONLY)
        install(FILES "${CMAKE_CURRENT_BINARY_DIR}/naudio-control.desktop"
                DESTINATION "${CMAKE_INSTALL_DATADIR}/applications" COMPONENT tools)
        # Its icon (issue #103): the desktop entry's Icon=naudio is resolved by the desktop's
        # icon theme through share/icons/hicolor/<size>x<size>/apps/naudio.png — a name, not a
        # path, so the theme picks the size the menu, the panel or the launcher wants. The
        # whole checked-in hicolor tree ships; the release gate asserts the sizes on the
        # installed system and that the entry's Icon= is the file's stem.
        if(NOT EXISTS "${NAUDIO_ICON_DIR}/hicolor/256x256/apps/naudio.png")
            message(FATAL_ERROR "naudio: missing icon tree ${NAUDIO_ICON_DIR}/hicolor")
        endif()
        install(DIRECTORY "${NAUDIO_ICON_DIR}/hicolor"
                DESTINATION "${CMAKE_INSTALL_DATADIR}/icons" COMPONENT tools
                FILES_MATCHING PATTERN "naudio.png")
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
# Its two POSIX time calls (a millisecond sleep and a monotonic clock) are SHIMMED for Windows,
# the same way the demo pair's are, so NAUDIO_BUILD_APPS alone is the right gate.
# ----------------------------------------------------------------------------
# It opens no device, so there is nothing platform-specific to exclude, and the remote end of an
# on-air check is frequently the operator's spare Windows box.
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
