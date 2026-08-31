# Per-generator CPack behavior — read by cpack AT PACKAGING TIME, once per
# generator run, with CPACK_GENERATOR set. A set() in CMakeLists.txt cannot
# vary by generator (every generator shares the one CPackConfig.cmake), so the
# scoping lives here, via CPACK_PROJECT_CONFIG_FILE.
#
# The double-clickable installers (macOS productbuild .pkg, Windows NSIS setup
# .exe) carry the runtime + tools components ONLY: their audience runs the
# tools and does not compile against the library. Headers, naudio.pc, the
# CMake package and the lib namelink stay in the archives (TGZ/ZIP) and the
# Linux packages, which remain monolithic full-tree images exactly as before —
# nothing in this file may touch a generator outside the two branches below
# (the release gates diff/assert the other packages' contents).

if(CPACK_GENERATOR STREQUAL "productbuild")
    set(CPACK_COMPONENTS_ALL runtime tools)
    # productbuild builds one inner pkg per component inside the single
    # product archive (grouping is not honored here — measured). Hide both
    # choices and mark them required: an installer whose audience is "make it
    # just work" asks no questions it doesn't need to.
    set(CPACK_COMPONENT_RUNTIME_HIDDEN TRUE)
    set(CPACK_COMPONENT_RUNTIME_REQUIRED TRUE)
    set(CPACK_COMPONENT_TOOLS_HIDDEN TRUE)
    set(CPACK_COMPONENT_TOOLS_REQUIRED TRUE)
    # Files land under /usr/local (bin/, lib/, share/) — the prefix macOS
    # convention expects, and the layout the tools' @loader_path/../lib
    # rpath already resolves. Scoped to this generator: the same variable on
    # TGZ would prepend usr/local/ inside the relocatable tarball.
    set(CPACK_PACKAGING_INSTALL_PREFIX "/usr/local")
    # The license/readme panes take only .rtf/.html/.txt/.rtfd — the repo's
    # extension-less LICENSE and README.md are both rejected, and productbuild
    # ERRORS on an unset readme rather than skipping the pane (measured). The
    # license .txt is staged at configure time; the readme is a short
    # pane-sized text written for the installer, not the repo README.
    set(CPACK_RESOURCE_FILE_LICENSE "${CPACK_NAUDIO_LICENSE_TXT}")
    set(CPACK_RESOURCE_FILE_README "${CPACK_NAUDIO_PKG_README_TXT}")
    # Registers the launchd agent after the payload lands (issue #96 item 2) — the
    # ".pkg postinstall" half of "registration rides the existing installers". Scoped
    # to this generator: the TGZ has no scripts, and the file is only defined when the
    # tools component (and so the daemon) is actually being built.
    if(CPACK_NAUDIO_POSTFLIGHT_SCRIPT)
        set(CPACK_POSTFLIGHT_TOOLS_SCRIPT "${CPACK_NAUDIO_POSTFLIGHT_SCRIPT}")
    endif()
elseif(CPACK_GENERATOR STREQUAL "NSIS")
    set(CPACK_COMPONENTS_ALL runtime tools)
    set(CPACK_COMPONENTS_GROUPING ALL_COMPONENTS_IN_ONE)
endif()
