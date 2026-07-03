cmake_minimum_required(VERSION 3.20)
project(naudio
        VERSION 0.1.0
        DESCRIPTION "Cross-platform C/C++ network-audio streaming toolkit: the 0xAF01 wire spec, multi-tenant server + clients, reliability stack (jitter/FEC/reorder/ARQ), device I/O, and a versioned C ABI"
        LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

option(NAUDIO_BUILD_TESTS "Build naudio unit tests" ON)
option(NAUDIO_BUILD_APPS  "Build naudio demo apps"  ON)

include(FetchContent)
include(GNUInstallDirs)

# Hidden default visibility everywhere: only symbols tagged NA_EXPORT (the na_* C
# ABI) end up in the shared library's dynamic symbol table. The internal C++
# classes (naudio_core / naudio_pa) are statically linked into the shared lib but
# stay hidden. (Static archives ignore visibility at link time, so the test/
# testbed targets that link the internals directly are unaffected.)
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

# Threads — needed by AudioRingBuffer's passive std::mutex/std::condition_variable
# (linked into naudio_core below). Resolved once here; the later naudio_net block
# re-requests it idempotently.
find_package(Threads REQUIRED)

# ----------------------------------------------------------------------------
# naudio_core — pure logic (classification, DUPLEX merge, virtual matching) +
# the wire codec (AudioPacket/ControlMessage/Crc32) + the reliability stack
# (FecEncoder/FecDecoder/ControlReliability/PacketReorderBuffer/JitterEstimator/
# AudioRingBuffer). No external deps, no hardware, no fork/exec, no PortAudio:
# fully unit-testable in CI. The reliability classes are compiled
# here rather than header-only, so the algorithms build once
# into libnaudio_core and ship as a stable library surface (avoids the
# ODR/recompile cost). PIC so it can be linked into the public shared library.
# ----------------------------------------------------------------------------
add_library(naudio_core STATIC
    src/Platform.cpp
    src/DeviceEnumerator.cpp
    src/FormatProbe.cpp
    src/StreamOpener.cpp
    src/ShellRunner.cpp
    src/VirtualAudioGuide.cpp
    src/PlatformConfigurator.cpp
    src/AudioPacket.cpp
    src/ControlMessage.cpp
    src/Crc32.cpp
    src/FecEncoder.cpp
    src/FecDecoder.cpp
    src/ControlReliability.cpp
    src/PacketReorderBuffer.cpp
    src/JitterEstimator.cpp
    src/AudioRingBuffer.cpp)
# BUILD/INSTALL_INTERFACE split: the build tree sees the source-tree
# include/, the installed export sees the install prefix's includedir — without
# this, install(EXPORT naudioTargets) would bake an absolute build-tree path into
# naudioTargets.cmake and fail to relocate.
target_include_directories(naudio_core PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
target_compile_features(naudio_core PUBLIC cxx_std_17)
set_target_properties(naudio_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
# AudioRingBuffer uses std::mutex + std::condition_variable (a thread-safe buffer,
# not a thread owner — the blocking read parks the caller). PUBLIC so consumers
# that pull AudioRingBuffer.o inherit the pthread link. naudio_core spawns no
# threads itself and uses no fork/exec, so it stays pure + portable; the
# per-client threads live in naudio_net.
target_link_libraries(naudio_core PUBLIC Threads::Threads)

# ----------------------------------------------------------------------------
# PortAudio — production backend dependency (the device I/O layer).
# Prefer a system / Homebrew install via pkg-config; fall back to FetchContent
# for CI hosts without it. NOTE: the FetchContent path is untested in this
# environment (the local build resolves PortAudio through pkg-config).
# ----------------------------------------------------------------------------
set(NAUDIO_PORTAUDIO_TARGET "")
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(PORTAUDIO QUIET IMPORTED_TARGET portaudio-2.0)
    if(TARGET PkgConfig::PORTAUDIO)
        set(NAUDIO_PORTAUDIO_TARGET PkgConfig::PORTAUDIO)
    endif()
endif()
if(NOT NAUDIO_PORTAUDIO_TARGET)
    message(STATUS "naudio: PortAudio not found locally; fetching v19.7.0 from source")
    # PortAudio v19.7.0 still ships cmake_minimum_required(VERSION <3.5), which
    # CMake >= 4.0 rejects outright (CMP0000) unless the policy floor is lowered.
    # Scope CMAKE_POLICY_VERSION_MINIMUM to just the fetched sub-build so naudio
    # keeps its own modern policy settings, then restore it. Without this the
    # no-system-PortAudio (FetchContent) path fails to configure on modern runners
    # (the path the FetchContent + Windows CI jobs exercise).
    if(DEFINED CMAKE_POLICY_VERSION_MINIMUM)
        set(NAUDIO_SAVED_POLICY_MIN "${CMAKE_POLICY_VERSION_MINIMUM}")
    endif()
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
    FetchContent_Declare(portaudio
        GIT_REPOSITORY https://github.com/PortAudio/portaudio.git
        GIT_TAG v19.7.0)
    FetchContent_MakeAvailable(portaudio)
    # Restore exactly: a set("") would leave the variable defined-but-empty for the
    # rest of the configure (the googletest FetchContent below sees it too).
    if(DEFINED NAUDIO_SAVED_POLICY_MIN)
        set(CMAKE_POLICY_VERSION_MINIMUM "${NAUDIO_SAVED_POLICY_MIN}")
        unset(NAUDIO_SAVED_POLICY_MIN)
    else()
        unset(CMAKE_POLICY_VERSION_MINIMUM)
    endif()
    # Prefer the STATIC PortAudio archive for the bundled build: PortAudio's shared
    # `portaudio` target builds a dylib that (on macOS) exports NO Pa_* symbols, so
    # linking it leaves every Pa_* undefined; the static `portaudio_static` archive
    # carries the symbols and is pulled in directly. Static-bundling is also the
    # right shape for a no-system-deps build — no build-tree dylib runtime dep.
    if(TARGET portaudio_static)
        set(NAUDIO_PORTAUDIO_TARGET portaudio_static)
        # PortAudio's own CMake never sets PIC on the static archive, but it gets
        # linked (via naudio_pa) into the SHARED naudio library — without this the
        # libnaudio.so link fails on Linux with R_X86_64_PC32 relocation errors.
        set_target_properties(portaudio_static PROPERTIES POSITION_INDEPENDENT_CODE ON)
    else()
        set(NAUDIO_PORTAUDIO_TARGET portaudio)
    endif()
endif()

# ----------------------------------------------------------------------------
# naudio_pa — internal C++ PortAudio backend (the device half). STATIC + PIC so
# it links both into the public shared `naudio` AND directly into internal C++
# consumers (the tools/ testbed), which use the C++ classes — NOT the C ABI — and
# therefore must NOT depend on the hidden-visibility shared library.
# ----------------------------------------------------------------------------
add_library(naudio_pa STATIC
    src/PortAudioBackend.cpp)
target_include_directories(naudio_pa PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
target_link_libraries(naudio_pa PUBLIC naudio_core ${NAUDIO_PORTAUDIO_TARGET})
set_target_properties(naudio_pa PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ----------------------------------------------------------------------------
# naudio_net — the transport/server/client layer. Sockets + per-client
# threads are platform-coupled the way naudio_pa's PortAudio is, so they live in
# their own STATIC + PIC target rather than polluting the pure naudio_core ("no
# threads, no fork/exec"). Backend-agnostic: takes a DeviceBackend*,
# never PortAudio directly. Links naudio_core (codec + reliability) + threads; on
# Windows the Winsock import library. Peer of naudio_pa above naudio_core. The
# public shared `naudio` links this (the na_client_* networking C ABI in
# naudio_c_api.cpp wraps AudioStreamClient).
# ----------------------------------------------------------------------------
find_package(Threads REQUIRED)
add_library(naudio_net STATIC
    src/net/Socket.cpp
    src/net/AudioProtocolHandler.cpp
    src/net/TcpServerTransport.cpp
    src/net/UdpClientConnection.cpp
    src/net/UdpServerTransport.cpp
    src/net/AudioBroadcaster.cpp
    src/net/AudioMixer.cpp
    src/net/AudioStreamServer.cpp
    src/net/AudioStreamClient.cpp)
target_include_directories(naudio_net PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
target_link_libraries(naudio_net PUBLIC naudio_core Threads::Threads)
set_target_properties(naudio_net PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(WIN32)
    target_link_libraries(naudio_net PUBLIC ws2_32)
endif()

# ----------------------------------------------------------------------------
# naudio — the PUBLIC shared library: the C ABI (naudio_c_api.cpp) over the
# internal backend. Versioned (SONAME from the project version) and exporting
# ONLY the na_* surface (hidden visibility + NA_EXPORT). This is the artifact a
# C / Hamlib consumer links against and the one `make install` ships.
# ----------------------------------------------------------------------------
add_library(naudio SHARED
    src/naudio_c_api.cpp)
target_include_directories(naudio PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
# naudio_pa = the device-layer backend; naudio_net = the transport/client layer the
# na_client_* C ABI wraps (AudioStreamClient). Both are STATIC + hidden-visibility, so only the
# NA_EXPORT na_* surface is exported from the shared object.
target_link_libraries(naudio PRIVATE naudio_pa naudio_net)
target_compile_definitions(naudio PRIVATE NAUDIO_BUILDING_SHARED)
set_target_properties(naudio PROPERTIES
    VERSION   ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR})

# ---- install / export -----------------------------------------------------
# Ship the full transport as an installable package. Two consumption models from
# ONE export set:
#   * C / Hamlib consumers -> link naudio::naudio (the hidden-visibility shared C
#                             ABI: na_client_* + na_server_*).
#   * C++ consumers        -> link naudio::naudio_core / naudio::naudio_net /
#                             naudio::naudio_pa (the internal classes, shipped as
#                             STATIC archives; static-link resolution ignores the
#                             hidden visibility the shared lib relies on, so the
#                             unexported C++ symbols stay callable). This is NOT an
#                             ABI-stable surface: the naudio:: headers embed STL
#                             types (std::string/map/mutex/atomic) directly in the
#                             class layouts, so the C++ path requires a matching
#                             toolchain + standard library (same compiler / STL
#                             ABI) as the archives were built with. The portable,
#                             ABI-stable path is the C na_* surface above; reach
#                             for the C++ targets only when you control both ends
#                             of the build.
# Both reachable via find_package(naudio) (CMake) or pkg-config (naudio.pc, C ABI).
#
# PortAudio note: naudio::naudio_pa references PkgConfig::PORTAUDIO in its install
# interface, so naudioConfig.cmake recreates it via pkg-config. This assumes a
# system/Homebrew PortAudio (the supported build path). PortAudio is already a
# runtime dependency of the naudio dylib (the bundled backend loads libportaudio),
# so requiring it at find_package time adds no new burden. Exporting against a
# FetchContent-built PortAudio is out of scope here (a CI concern).
include(CMakePackageConfigHelpers)

# Portable thread lib for the pkg-config static-link closure: CMAKE_THREAD_LIBS_INIT
# is "-lpthread" where a separate lib is needed and empty where it is folded into libc.
set(NAUDIO_PC_PRIVATE_LIBS "${CMAKE_THREAD_LIBS_INIT}")
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/naudio.pc.in"
               "${CMAKE_CURRENT_BINARY_DIR}/naudio.pc" @ONLY)

# All four targets relocate together under one export set, so a C++ consumer
# linking naudio::naudio_net pulls the transitive naudio::naudio_core / Threads
# automatically.
install(TARGETS naudio naudio_core naudio_pa naudio_net
    EXPORT naudioTargets
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")

# Public headers: the top-level C ABI header + the entire C++ API tree (naudio/**,
# incl. export.h and net/). install(DIRECTORY) preserves the naudio/ and
# naudio/net/ subdirs the #includes expect.
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/include/naudio.h"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include/naudio"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
        FILES_MATCHING PATTERN "*.hpp" PATTERN "*.h")

install(FILES "${CMAKE_CURRENT_BINARY_DIR}/naudio.pc"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig")

# CMake package config: naudioTargets.cmake (relocatable target definitions) +
# naudioConfig.cmake (recreates the Threads/PortAudio imported deps the C++ targets
# reference, then includes the targets file) + a SameMajorVersion version file.
set(NAUDIO_CMAKE_CONFIG_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/naudio")
install(EXPORT naudioTargets
    FILE naudioTargets.cmake
    NAMESPACE naudio::
    DESTINATION "${NAUDIO_CMAKE_CONFIG_DESTINATION}")
configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/naudioConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/naudioConfig.cmake"
    INSTALL_DESTINATION "${NAUDIO_CMAKE_CONFIG_DESTINATION}")
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/naudioConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/naudioConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/naudioConfigVersion.cmake"
    DESTINATION "${NAUDIO_CMAKE_CONFIG_DESTINATION}")

# ----------------------------------------------------------------------------
# Tests — GoogleTest (system/Homebrew preferred, FetchContent fallback).
# Guarded by EXISTS so partial checkouts still configure.
# ----------------------------------------------------------------------------
if(NAUDIO_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    find_package(GTest CONFIG QUIET)
    if(NOT GTest_FOUND)
        message(STATUS "naudio: GoogleTest not found locally; fetching v1.15.2 from source")
        FetchContent_Declare(googletest
            GIT_REPOSITORY https://github.com/google/googletest.git
            GIT_TAG v1.15.2)
        set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
        # Keep GoogleTest's own install rules out of `make install` so our prefix
        # ships only libnaudio + the C headers + naudio.pc (not gtest/gmock).
        set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(googletest)
    endif()
    enable_testing()
    add_subdirectory(tests)

    # C-ABI contract smoke: a C translation unit (compiled as C, linked as C++)
    # that proves naudio.h is C-callable and checks the documented NULL-buffer / truncation contract
    # of the text functions (na_install_instructions used to segfault on a NULL buffer). This
    # preserves the C-callability coverage the removed na_guide/na_loopback demos provided. Links
    # the full naudio (PortAudio) library.
    add_executable(naudio_c_abi_smoke tests/c_abi_smoke.c)
    target_link_libraries(naudio_c_abi_smoke PRIVATE naudio)
    set_target_properties(naudio_c_abi_smoke PROPERTIES LINKER_LANGUAGE CXX)
    add_test(NAME naudio_c_abi_smoke COMMAND naudio_c_abi_smoke)

    # Networking C-ABI smoke: a PURE-C client (na_client_*) driving a full connect ->
    # RX-frame-via-callback -> disconnect against an in-process C++ AudioStreamServer over loopback,
    # hardware-free (NULL client backend + inject-only server, no PortAudio). The .c proves the
    # na_client_* surface is C-compilable/callable; net_smoke_server.cpp is a C++ test fixture
    # (extern "C") the C side calls to spin up the loopback server. Links naudio (the public C ABI)
    # + naudio_net (the server fixture's AudioStreamServer) + Threads.
    add_executable(naudio_c_net_smoke tests/c_net_smoke.c tests/net_smoke_server.cpp)
    target_link_libraries(naudio_c_net_smoke PRIVATE naudio naudio_net Threads::Threads)
    set_target_properties(naudio_c_net_smoke PROPERTIES LINKER_LANGUAGE CXX)
    add_test(NAME naudio_c_net_smoke COMMAND naudio_c_net_smoke)

    # Server C-ABI smoke: a PURE-C harness driving BOTH ends over loopback —
    # the na_server_* surface creates/starts an inject-only NULL-backend server and a real na_client_*
    # NULL-backend client connects to it (RX inject -> client callback, roster, TX-extract callback,
    # clean disconnect+stop). Unlike c_net_smoke, no C++ fixture is needed: the na_server_* ABI lets
    # the C side own the server too. Links only the public C ABI (`naudio`); na_server_*/na_client_*
    # both live there, which already pulls in naudio_net privately.
    add_executable(naudio_c_server_smoke tests/c_server_smoke.c)
    target_link_libraries(naudio_c_server_smoke PRIVATE naudio)
    set_target_properties(naudio_c_server_smoke PROPERTIES LINKER_LANGUAGE CXX)
    add_test(NAME naudio_c_server_smoke COMMAND naudio_c_server_smoke)

    # C-ABI fault-injection: a C++ TU (a global operator new override can't be done from C)
    # that arms allocation failure around single na_client_* calls and asserts the extern "C"
    # boundary stays exception-tight: a forced bad_alloc surfaces as NA_ERR_NOMEM (not a thrown
    # exception), a getter clears the per-thread error on entry, callbacks are rejected after
    # connect, and na_client_destroy under OOM does not std::terminate. Links the public C ABI
    # only (na_client_* lives in `naudio`, which already pulls in naudio_net privately).
    # Not on Windows: the fault injection replaces global operator new in the TEST
    # executable, but the na_* allocations happen inside naudio.dll, which resolved
    # its own operator new at link time — the interposition never fires across a DLL
    # boundary (it does on Linux/macOS via ELF preemption / weak-def coalescing).
    if(NOT WIN32)
        add_executable(naudio_c_abi_fault tests/c_abi_fault.cpp)
        target_link_libraries(naudio_c_abi_fault PRIVATE naudio)
        add_test(NAME naudio_c_abi_fault COMMAND naudio_c_abi_fault)
    endif()

    # README honesty gate: extract every ```c / ```cpp
    # fenced block from README.md and compile each (-fsyntax-only) against the
    # in-tree headers, so a documented example that stops matching the real C ABI /
    # C++ API fails CI. POSIX-only (a bash + cc/c++ harness); the headers it checks
    # are platform-neutral, so running it on the Linux + macOS matrix covers them.
    if(NOT WIN32)
        add_test(NAME naudio_readme_snippets
                 COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/readme-snippets/check.sh")
    endif()
endif()

# ----------------------------------------------------------------------------
# tools/ — na_audio_daemon: the hardware-smoke driver. Deliberately OUTSIDE
# naudio_core/naudio: an exerciser (it runs the real AudioStreamServer /
# AudioStreamClient over a live PortAudio device) rather than an audio primitive
# (keeps the core pure + portable). Added last so it can reuse the
# GoogleTest targets configured by the tests block. Excluded on Windows — the
# Windows CI job builds the libraries + C ABI + tests lib-only; the daemon is a
# hardware smoke CI never runs.
# ----------------------------------------------------------------------------
if(NOT WIN32)
    add_subdirectory(tools)
endif()

# ----------------------------------------------------------------------------
# examples/ — reference consumers of the PUBLIC na_* C ABI (the play-to-speakers
# clients in C/C++/Python/Java/Rust and the demo source). These link ONLY the public
# shared `naudio` and include only naudio.h — the proof the C ABI is self-sufficient
# from pure C. Added after the `naudio` shared target is defined; gated internally by
# NAUDIO_BUILD_APPS.
# ----------------------------------------------------------------------------
add_subdirectory(examples)
