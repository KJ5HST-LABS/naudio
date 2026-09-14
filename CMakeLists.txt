cmake_minimum_required(VERSION 3.20)
project(naudio
        VERSION 1.0.0
        DESCRIPTION "Cross-platform C/C++ network-audio streaming toolkit: the 0xAF01 wire spec, multi-tenant server + clients, reliability stack (jitter/FEC/reorder/ARQ), device I/O, and a versioned C ABI"
        LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ----------------------------------------------------------------------------
# C standard (issue #42).
#
# C is this project's DELIVERABLE language -- include/naudio.h is a C ABI -- yet its level was left
# to whatever each toolchain happened to default to, while C++ above was pinned deliberately. That
# asymmetry WAS the bug: clang and gcc default high enough to accept the C11 _Static_assert and
# _Alignas in tests/c_abi_smoke.c, MSVC does not, and the Windows build stayed broken for 54 commits
# because no local toolchain could see it.
#
# C11 is not a new choice made here -- it is what tests/external-consumer/CMakeLists.txt,
# tests/ffi-clients/run.sh and tests/readme-snippets/check.sh already compile at. This is simply the
# fourth site, and the only one that never said so.
#
# This level is what the TESTS need, not what the header demands of a consumer: include/naudio.h is
# C99-clean (measured with -pedantic-errors; its floor is `long long`, naudio.h:569-570). A consumer
# is free to compile at C99. That sentence is no longer a measurement someone took once -- the
# `naudio_c99_header_gate` ctest arm re-checks it on every run (tests/c99-header-gate/, issue #45),
# because every other gate compiling C against this header does so at C11 or higher and so none of
# them can see the floor rise.
#
# CMAKE_C_EXTENSIONS is deliberately NOT set OFF, and the asymmetry with CMAKE_CXX_EXTENSIONS above
# is intentional -- please do not "fix" it. Strict -std=c11 defines __STRICT_ANSI__, which on glibc
# hides every POSIX declaration behind a feature-test macro; tools/na_hamlib_bridge.c defines none
# and calls clock_gettime/nanosleep/pthread_*/getopt. Measured on exactly that header+call set:
# -std=c11 COMPILES on Apple libc and FAILS on glibc ('CLOCK_MONOTONIC' undeclared,
# clock_gettime/nanosleep implicit). Setting it OFF would therefore look correct on a macOS laptop
# and break the Linux bridge job -- the same platform-blind failure this issue is about. C++ escapes
# this because libstdc++'s own headers define _GNU_SOURCE; C has no equivalent.
# ----------------------------------------------------------------------------
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# No MSVC C11-atomics opt-in here, deliberately, and the record is worth keeping (issue #28).
#
# The C smoke tests need an atomic qualifier for the flags their callbacks set on naudio's worker
# threads — TSan proved `volatile` is not sufficient. MSVC gates C11 atomics behind
# /experimental:c11atomics even at /std:c11, and <stdatomic.h> is a hard
# `#error "C atomic support is not enabled"` without it. That flag was delivered from here TWICE —
# first as add_compile_options($<$<COMPILE_LANGUAGE:C>:...>), then as a plain
# string(APPEND CMAKE_C_FLAGS ...) with no generator in the middle — and windows-latest / MSVC
# 14.51 returned the BYTE-IDENTICAL error to all four targets each time. Two failed deliveries with
# no observable difference means the mechanism was never established, and a third guess against a
# toolchain nobody here can run is not a measurement.
#
# The dependency is removed instead: tests/c_atomic_compat.h picks the qualifier per compiler, so
# Windows keeps the `volatile` it already had and every platform the sanitizers actually run on
# gets _Atomic. See that header for the residual exposure.

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

option(NAUDIO_BUILD_TESTS "Build naudio unit tests" ON)
option(NAUDIO_BUILD_APPS  "Build naudio demo apps"  ON)
# Force the FetchContent PortAudio (static, PIC, linked INTO libnaudio) even when a
# system PortAudio exists. This is the release-packaging switch (issue #93): a binary
# package must not change shape with the builder's environment — a runner image that
# happens to grow a system PortAudio would otherwise silently turn a self-contained
# libnaudio into one referencing a Homebrew/apt dylib the user may not have. OFF for
# development, where the system path is the supported default.
option(NAUDIO_BUNDLE_PORTAUDIO
       "Ignore any system PortAudio and build the bundled static one into libnaudio" OFF)

include(FetchContent)
include(GNUInstallDirs)

# ----------------------------------------------------------------------------
# Installed-binary rpath (issue #93). The installed tools (na_wav_tap,
# na_hamlib_bridge) link the SHARED libnaudio, which lands in <prefix>/lib while
# they land in <prefix>/bin. CMake strips the build rpath at install time, so
# without this an installed tool resolves libnaudio only through the loader's
# default search path — which works for a /usr package and silently fails for a
# relocatable tarball untarred anywhere else. A relative rpath makes the
# installed tree self-locating wherever it is unpacked. Guarded so a packager
# who sets a policy of their own (or strips rpaths entirely) is not overridden.
# ----------------------------------------------------------------------------
if(NOT DEFINED CMAKE_INSTALL_RPATH)
    if(APPLE)
        set(CMAKE_INSTALL_RPATH "@loader_path/../${CMAKE_INSTALL_LIBDIR}")
    elseif(UNIX)
        set(CMAKE_INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")
    endif()
endif()

# ----------------------------------------------------------------------------
# Version drift gate (issue #31).
#
# include/naudio.h carries NAUDIO_VERSION_MAJOR/MINOR/PATCH as HAND-WRITTEN macros. The public
# header is deliberately generated by nothing — see include/naudio/export.h's note on not using
# generate_export_header, which keeps the C ABI header self-contained and the build offline-clean —
# so nothing derives those macros from the project(VERSION) above.
#
# That makes them a FOURTH place the version lives, beside the SONAME, naudio.pc's Version: and
# naudioConfigVersion.cmake, and the only one of the four that is not derived. Left ungated, a
# consumer comparing NAUDIO_VERSION_NUMBER against na_version_number() would get a CONFIRMED WRONG
# answer rather than no answer — strictly worse than shipping no accessor at all, and invisible in
# a diff that touches only one side. Hence a configure-time FATAL_ERROR: drift must be unbuildable,
# not merely reported by a test somebody can skip.
#
# CMAKE_CONFIGURE_DEPENDS is load-bearing: without it, editing the header alone would not re-run
# configure and the gate would silently not fire on exactly the edit it exists to catch.
# ----------------------------------------------------------------------------
set_property(DIRECTORY APPEND PROPERTY
             CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/include/naudio.h")
foreach(_na_part MAJOR MINOR PATCH)
    file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/include/naudio.h" _na_line
         REGEX "^#define[ \t]+NAUDIO_VERSION_${_na_part}[ \t]+[0-9]+[ \t]*$")
    if(NOT _na_line)
        message(FATAL_ERROR
            "naudio version gate: include/naudio.h defines no NAUDIO_VERSION_${_na_part}. "
            "It is the compile-time half of the na_version_* accessors and must exist.")
    endif()
    string(REGEX REPLACE "^#define[ \t]+NAUDIO_VERSION_${_na_part}[ \t]+([0-9]+).*$" "\\1"
           _na_${_na_part} "${_na_line}")
endforeach()
set(NAUDIO_HEADER_VERSION "${_na_MAJOR}.${_na_MINOR}.${_na_PATCH}")
if(NOT NAUDIO_HEADER_VERSION VERSION_EQUAL PROJECT_VERSION)
    message(FATAL_ERROR
        "naudio version drift: include/naudio.h says ${NAUDIO_HEADER_VERSION}, project(VERSION) "
        "says ${PROJECT_VERSION}. These must match and must be bumped in the SAME commit — the "
        "header macros are what a consumer compares against na_version_number(), while "
        "PROJECT_VERSION sets the SONAME and naudio.pc's Version:.")
endif()
# NA_VERSION_ENCODE keeps the ordering total only for components within NA_VERSION_MAX_COMPONENT.
# That bound is READ FROM THE HEADER rather than spelled here, so this check cannot disagree with
# the macro it is enforcing -- the same drift this whole gate exists to prevent, one level down.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/include/naudio.h" _na_max_line
     REGEX "^#define[ \t]+NA_VERSION_MAX_COMPONENT[ \t]+[0-9]+[ \t]*$")
if(NOT _na_max_line)
    message(FATAL_ERROR
        "naudio version gate: include/naudio.h defines no NA_VERSION_MAX_COMPONENT, which bounds "
        "NA_VERSION_ENCODE's components and is what this check enforces.")
endif()
string(REGEX REPLACE "^#define[ \t]+NA_VERSION_MAX_COMPONENT[ \t]+([0-9]+).*$" "\\1"
       _na_max "${_na_max_line}")
if(_na_MINOR GREATER _na_max OR _na_PATCH GREATER _na_max)
    message(FATAL_ERROR
        "naudio version gate: ${NAUDIO_HEADER_VERSION} exceeds NA_VERSION_MAX_COMPONENT "
        "(${_na_max}), so NA_VERSION_ENCODE would let a lower component carry into a higher one "
        "and every consumer comparison would be wrong at that boundary. Widen the encoding first.")
endif()
unset(_na_line)
unset(_na_max_line)

# Hidden default visibility everywhere: only symbols tagged NA_EXPORT (the na_* C
# ABI) end up in the shared library's dynamic symbol table. The internal C++
# classes (naudio_core / naudio_pa) are statically linked into the shared lib but
# stay hidden. (Static archives ignore visibility at link time, so the test/
# testbed targets that link the internals directly are unaffected.)
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

# ----------------------------------------------------------------------------
# NAUDIO_SANITIZE — opt-in compiler instrumentation (issue #28).
#
# Off by default and inert when off: everything below is inside the if(), so a normal build sees
# no added flag. On, it instruments EVERY in-tree target -- which is why this block sits above the
# first add_library(): add_compile_options/add_link_options apply only to targets created after
# them, so moving it below any target silently un-instruments that target. That is the failure this
# whole issue is about (an instrument that is present but not watching), so the position is
# load-bearing, not stylistic.
#
# Accepts a comma list because ASan and UBSan compose and are meant to be run together; `thread` is
# rejected alongside `address` because the two runtimes cannot coexist in one process.
#
# -fno-sanitize-recover=all is the non-obvious one, and it is MEASURED, not assumed. UBSan is
# RECOVERABLE by default: it prints `runtime error: ...` and the process still exits 0. Measured on
# Apple clang 21 with a runtime-valued `1 << 33` -- default build exits 0, the same binary with
# -fno-sanitize-recover=all exits 134. ctest keys on the exit code, so without this flag a UBSan
# job reports PASS on every run while printing findings into a log nobody reads. ASan and TSan
# abort on their own (both measured at 134 here, since Darwin defaults abort_on_error=1), so the
# flag exists for UBSan's sake and is harmless to the others.
#
# Leaks are deliberately NOT part of this option: LSan rides along with ASan on Linux only.
# Measured here -- `ASAN_OPTIONS=detect_leaks=1` on arm64 Darwin prints "detect_leaks is not
# supported on this platform". So the leak half of this issue is reachable only in CI, and no
# local ASan run should ever be reported as leak coverage.
# ----------------------------------------------------------------------------
set(NAUDIO_SANITIZE "" CACHE STRING
    "Sanitizers to instrument the build with: address, undefined, thread, or a comma list \
(e.g. address,undefined). Empty (the default) builds with no instrumentation.")
set_property(CACHE NAUDIO_SANITIZE PROPERTY STRINGS "" address undefined thread address,undefined)

if(NAUDIO_SANITIZE)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        # A FATAL_ERROR rather than a warning-and-continue: MSVC spells this /fsanitize=address and
        # supports neither thread nor undefined, so accepting the option there would produce an
        # uninstrumented build that reports itself as sanitized -- issue #28's own failure mode.
        message(FATAL_ERROR
            "NAUDIO_SANITIZE=${NAUDIO_SANITIZE} is not supported for ${CMAKE_CXX_COMPILER_ID}. "
            "-fsanitize= is a Clang/GCC spelling; configure without NAUDIO_SANITIZE on this "
            "toolchain.")
    endif()

    string(REPLACE "," ";" _na_san_list "${NAUDIO_SANITIZE}")
    foreach(_na_san IN LISTS _na_san_list)
        if(NOT _na_san MATCHES "^(address|undefined|thread)$")
            message(FATAL_ERROR
                "NAUDIO_SANITIZE: unknown sanitizer '${_na_san}'. Accepted: address, undefined, "
                "thread, or a comma list of them.")
        endif()
    endforeach()
    if("thread" IN_LIST _na_san_list AND "address" IN_LIST _na_san_list)
        message(FATAL_ERROR
            "NAUDIO_SANITIZE: 'address' and 'thread' cannot be combined -- the two runtimes are "
            "mutually exclusive in one process. Configure two build dirs and run them separately, "
            "as .github/workflows/ci.yml does.")
    endif()

    # -fno-omit-frame-pointer and -g are what make a report name a naudio frame instead of an
    # address: without them a use-after-free in a detached thread points at nothing actionable.
    set(_na_san_flags -fsanitize=${NAUDIO_SANITIZE} -fno-omit-frame-pointer -g)
    if("undefined" IN_LIST _na_san_list)
        list(APPEND _na_san_flags -fno-sanitize-recover=all)
        # SUPPRESSION 1 of 1, and the justification is required to stay attached to it (issue #28).
        #
        # -fsanitize=enum flags a load of an enum object holding a value outside its enumerator
        # range. Three sites hit it, and all three are the C ABI VALIDATING UNTRUSTED INPUT: a C
        # caller may legally pass any integer, so na_strerror() (naudio_c_api.cpp:142),
        # na_client_set_reliability_profile() (:684) and na_server_set_reliability_profile()
        # (:1191) switch on the parameter and reject the unknown value in `default:`. The check
        # therefore fires on precisely the code that is being defensive, and the tests that trip it
        # exist to prove that defence works — c_abi_smoke.c:166 passes (na_error_t)(-999), and
        # c_client_profile.c:164 passes (na_reliability_profile)99 to each of the two setters.
        #
        # MEASURED, not assumed: both arms PASS in the normal optimized build, so the `default:`
        # branches do execute on today's toolchains and the ABI really does reject these values.
        # The exposure is latent — a compiler is permitted to assume the parameter is in range and
        # elide the branch — so this is a real standards defect and not noise. It is recorded HERE
        # and in CHANGELOG.md rather than tracked as an issue, because this comment is the thing a
        # future reader will actually meet. Deleting this one flag is how you get the finding back;
        # if a toolchain ever does elide one of those branches, that is the moment it earns an
        # issue and a laundered-through-an-integer read at the three sites named above.
        list(APPEND _na_san_flags -fno-sanitize=enum)
    endif()
    add_compile_options(${_na_san_flags})
    add_link_options(${_na_san_flags})
    message(STATUS "naudio: sanitizers ENABLED -- ${_na_san_flags}")
endif()

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
    src/AudioRingBuffer.cpp
    src/FormatConversion.cpp)
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
if(NOT NAUDIO_BUNDLE_PORTAUDIO)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(PORTAUDIO QUIET IMPORTED_TARGET portaudio-2.0)
        if(TARGET PkgConfig::PORTAUDIO)
            set(NAUDIO_PORTAUDIO_TARGET PkgConfig::PORTAUDIO)
        endif()
    endif()
endif()
if(NOT NAUDIO_PORTAUDIO_TARGET)
    if(NAUDIO_BUNDLE_PORTAUDIO)
        message(STATUS "naudio: NAUDIO_BUNDLE_PORTAUDIO=ON — ignoring any system PortAudio; "
                       "fetching v19.7.0 from source")
    else()
        message(STATUS "naudio: PortAudio not found locally; fetching v19.7.0 from source")
    endif()
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
    # Keep PortAudio's own install rules out of `cmake --install` so our prefix ships
    # only naudio — the exact analogue of INSTALL_GTEST OFF below, which PortAudio had
    # no counterpart for. Measured before this line existed: a FetchContent install
    # additionally deposited portaudio.h, pa_mac_core.h, libportaudio.a, libportaudio
    # .dylib, lib/cmake/portaudio/*, lib/pkgconfig/portaudio-2.0.pc and
    # share/doc/portaudio/* into the naudio prefix (issue #61).
    #
    # This is load-bearing in BOTH directions, and the two halves must land together:
    # disabling PortAudio's install also removes it from any export set, so if
    # naudio_pa still named a PortAudio target in its INSTALL interface, install(EXPORT
    # naudioTargets) would hard-fail at generate time. The $<BUILD_INTERFACE:> below is
    # what makes this safe.
    set(PA_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
    FetchContent_Declare(portaudio
        GIT_REPOSITORY https://github.com/PortAudio/portaudio.git
        GIT_TAG v19.7.0)
    FetchContent_MakeAvailable(portaudio)
    # Which resolution path we took — read by the naudio.pc generation below, where a
    # bundled PortAudio must NOT be advertised as an external pkg-config module.
    set(NAUDIO_PORTAUDIO_BUNDLED TRUE)
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
# PortAudio is linked in the BUILD interface ONLY. Without the generator expression
# the build-time target NAME is baked into the installed naudioTargets.cmake — either
# `PkgConfig::PORTAUDIO` or the bare `portaudio_static`, depending on how this machine
# resolved it — and naudioConfig.cmake must then recreate that exact name. It could
# only ever recreate one of the two, and only via pkg-config, which is why
# find_package(naudio) failed two different ways before this line (issue #61):
#   * on MSVC (no pkg-config at all)  -> "dependency PkgConfig could not be found"
#   * on a FetchContent-built install -> "imported targets are referenced, but are
#                                         missing: portaudio_static"
# With the install interface carrying no PortAudio target, naudioConfig.cmake resolves
# PortAudio on the CONSUMER's terms and attaches it to naudio::naudio_pa there.
target_link_libraries(naudio_pa PUBLIC naudio_core "$<BUILD_INTERFACE:${NAUDIO_PORTAUDIO_TARGET}>")
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
    src/net/AudioStreamClient.cpp
    src/net/Discovery.cpp)
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
# PortAudio note: the installed package names NO PortAudio target (the link above is
# $<BUILD_INTERFACE:>-scoped), so this export is independent of how THIS machine
# resolved PortAudio — system pkg-config or FetchContent both produce the same
# naudioTargets.cmake. naudioConfig.cmake resolves PortAudio on the consumer's terms
# (pkg-config, else a plain find_library/find_path) and attaches it to
# naudio::naudio_pa, the only target that needs it. When nothing is found the package
# still loads: naudio::naudio, the shared C ABI, has its backend linked in already and
# is the surface a C / Hamlib consumer uses. Only the C++ naudio::naudio_pa archive
# needs a consumer-side PortAudio. (Issue #61; exporting against a FetchContent-built
# PortAudio was previously waved off here as "out of scope, a CI concern" — it was the
# defect.)
include(CMakePackageConfigHelpers)

# The pkg-config static-link closure, as literal linker flags.
#
# naudio.pc carries NO Requires.private, deliberately (issue #61). pkg-config resolves
# Requires.private on EVERY query, not just --static, so a module named there that is
# not installed makes plain `pkg-config --cflags naudio` and `--libs naudio` exit 1
# (measured, pkgconf 2.5.1). `Requires.private: portaudio-2.0` therefore broke the .pc
# two ways: on a bundled build the module does not exist anywhere and the claim was
# simply false, and on a system build any consumer without PortAudio's DEVELOPMENT
# files hit the same hard error — while needing none of it, since naudio.pc describes
# the SHARED libnaudio, whose PortAudio dependency the dynamic linker already carries.
# For Hamlib upstream that is a build failure over a dependency Hamlib never uses.
#
# Libs.private is raw flags and triggers no module lookup, so the .pc degrades instead
# of erroring on every host. CMAKE_THREAD_LIBS_INIT is "-lpthread" where a separate lib
# is needed and empty where it is folded into libc (e.g. macOS).
set(NAUDIO_PC_PRIVATE_LIBS "${CMAKE_THREAD_LIBS_INIT}")
if(NOT NAUDIO_PORTAUDIO_BUNDLED)
    # System PortAudio: record its static closure for `pkg-config --static`. Prefer the
    # STATIC flag set — it carries the platform audio libraries (CoreAudio/AudioToolbox
    # frameworks, libasound/libjack) that plain --libs omits. A bundled PortAudio needs
    # no entry at all: it is linked INTO libnaudio.
    if(PORTAUDIO_STATIC_LDFLAGS)
        set(NAUDIO_PA_PC_LIBS "${PORTAUDIO_STATIC_LDFLAGS}")
    else()
        set(NAUDIO_PA_PC_LIBS "${PORTAUDIO_LDFLAGS}")
    endif()
    if(NAUDIO_PA_PC_LIBS)
        string(REPLACE ";" " " NAUDIO_PA_PC_LIBS "${NAUDIO_PA_PC_LIBS}")
        string(STRIP "${NAUDIO_PC_PRIVATE_LIBS} ${NAUDIO_PA_PC_LIBS}" NAUDIO_PC_PRIVATE_LIBS)
    endif()
endif()
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/naudio.pc.in"
               "${CMAKE_CURRENT_BINARY_DIR}/naudio.pc" @ONLY)

# All four targets relocate together under one export set, so a C++ consumer
# linking naudio::naudio_net pulls the transitive naudio::naudio_core / Threads
# automatically.
# Component layout (issue #93): `runtime` = the shared C ABI library, `dev` =
# headers + static archives + pkg-config + CMake package, `tools` = the installed
# executables (tools/CMakeLists.txt). A plain `cmake --install` still installs
# everything; the tags let a packager split, and put the .so/.dylib namelink with
# the development files where DEB/RPM conventions expect it.
install(TARGETS naudio naudio_core naudio_pa naudio_net
    EXPORT naudioTargets
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
            COMPONENT runtime NAMELINK_COMPONENT dev
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT dev
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT runtime)

# Public headers: the top-level C ABI header + the entire C++ API tree (naudio/**,
# incl. export.h and net/). install(DIRECTORY) preserves the naudio/ and
# naudio/net/ subdirs the #includes expect.
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/include/naudio.h"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}" COMPONENT dev)
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include/naudio"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}" COMPONENT dev
        FILES_MATCHING PATTERN "*.hpp" PATTERN "*.h")

install(FILES "${CMAKE_CURRENT_BINARY_DIR}/naudio.pc"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig" COMPONENT dev)

# CMake package config: naudioTargets.cmake (relocatable target definitions) +
# naudioConfig.cmake (recreates the Threads/PortAudio imported deps the C++ targets
# reference, then includes the targets file) + a SameMajorVersion version file.
set(NAUDIO_CMAKE_CONFIG_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/naudio")
install(EXPORT naudioTargets
    FILE naudioTargets.cmake
    NAMESPACE naudio::
    DESTINATION "${NAUDIO_CMAKE_CONFIG_DESTINATION}"
    COMPONENT dev)
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
    DESTINATION "${NAUDIO_CMAKE_CONFIG_DESTINATION}"
    COMPONENT dev)

# ----------------------------------------------------------------------------
# Licence text, the wire spec, and the conformance vectors — installed so they
# travel WITH the artifact rather than only living in the repository.
#
# Every one of these was measured ABSENT from all six shipped artifact forms
# (.deb, .rpm, macOS .pkg, both .tar.gz, the .zip) on 2026-08-31. Two distinct
# defects were hiding behind the same gap:
#
#  * LEGAL. naudio is LGPL-2.1-or-later, and five of the six artifacts carried no
#    licence text at all. Debian Policy 12.5 makes /usr/share/doc/<pkg>/copyright
#    MANDATORY, so the .deb was non-compliant. CPACK_RESOURCE_FILE_LICENSE is set
#    (see the CPack block below) and looks like it covers this — it does not. It
#    is honoured by the NSIS and productbuild generators, which show a licence
#    PAGE at install time, and IGNORED by the archive, DEB and RPM generators. A
#    CPack resource variable is per-generator; setting it once does not make the
#    property true everywhere, which is why this needs real install() rules.
#
#  * PROTOCOL. Section 12 of the spec calls the golden vectors "the gate every
#    reference client must pass" — and neither the spec nor the vectors shipped,
#    so an implementer holding a release could reach neither. Shipping them makes
#    the artifact self-sufficient: spec + known-answer vectors + headers is
#    everything a third party needs to write a compliant client and prove it.
#
# The licence is named `copyright` because that is the filename Debian Policy
# REQUIRES; every other platform treats it as an ordinary doc file, so one file
# satisfies the hard requirement and the conventions together.
#
# Component split: the licence and notices ride `runtime`, because they must
# accompany the binaries themselves. The spec and vectors ride `dev`, because
# they are what someone BUILDING against naudio (or re-implementing the wire)
# needs. A plain `cmake --install` still installs all of it.
# ----------------------------------------------------------------------------
set(NAUDIO_DOC_DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/doc/naudio")

install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
        DESTINATION "${NAUDIO_DOC_DESTINATION}"
        RENAME copyright
        COMPONENT runtime)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY_NOTICES.md"
        DESTINATION "${NAUDIO_DOC_DESTINATION}"
        COMPONENT runtime)

install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/docs/audio-streaming-protocol-v1.md"
              "${CMAKE_CURRENT_SOURCE_DIR}/docs/protocols.md"
        DESTINATION "${NAUDIO_DOC_DESTINATION}"
        COMPONENT dev)

# The vectors keep their repo-relative shape (vectors/, tools/) so the paths the
# spec and conformance/README.md name still read correctly once installed.
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/conformance/README.md"
        DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/naudio/conformance"
        COMPONENT dev)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/conformance/vectors/vectors.ini"
              "${CMAKE_CURRENT_SOURCE_DIR}/conformance/vectors/vectors-v1_2.ini"
              "${CMAKE_CURRENT_SOURCE_DIR}/conformance/vectors/vectors-v1_4.ini"
        DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/naudio/conformance/vectors"
        COMPONENT dev)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/conformance/tools/gen_vectors.py"
        DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/naudio/conformance/tools"
        COMPONENT dev)

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

    # Issue #88 item 3 (from #60). Both arms below can report SKIPPED, and that is the point: this
    # test used to narrate its own degradation on stdout, which ctest never shows for a passing
    # test (`--output-on-failure` is used by every job), so the discriminating line reached no CI
    # log ever. A skip is carried in the test RESULT and appears in the summary either way.
    #
    # 77 is the autotools convention and must stay in step with NA_SKIP_RC in tests/c_abi_smoke.c.
    #
    # The second arm exists rather than a flag on the first because the contract checks are
    # device-independent: on a machine with 0 devices they still carry their full weight, and
    # marking the whole test skipped there would discard real coverage in order to report the loss
    # of some. It re-runs the same ~0.03 s binary and changes only the exit-code policy at the
    # stride arm, so a run that cannot distinguish the caller's stride from the library's says so
    # in the summary instead of passing.
    add_test(NAME naudio_c_abi_stride COMMAND naudio_c_abi_smoke --require-stride)
    set_tests_properties(naudio_c_abi_smoke naudio_c_abi_stride PROPERTIES SKIP_RETURN_CODE 77)

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

    # The FAILURE-path lifecycle contract (issue #58), which the smokes above do not reach: they
    # drive the success path, and every defect this pins lived in what happens after a call
    # FAILS. Asserts naudio.h's stated rule — NA_ERR_BACKEND means the attempt failed and the
    # handle is still usable, NA_ERR_INVALID means the handle is spent — against a real bind
    # collision and a real full-roster rejection, each with a control so a green cannot mean
    # "nothing works any more". Public C ABI only; hardware-free.
    add_executable(naudio_c_lifecycle_contract tests/c_lifecycle_contract.c)
    target_link_libraries(naudio_c_lifecycle_contract PRIVATE naudio)
    set_target_properties(naudio_c_lifecycle_contract PROPERTIES LINKER_LANGUAGE CXX)
    add_test(NAME naudio_c_lifecycle_contract COMMAND naudio_c_lifecycle_contract)

    # Sustained-TX regression: the same two ends as c_server_smoke, but over UDP and asserting
    # TX audio is still flowing in the SECOND HALF of a multi-second run. c_net_smoke's TX
    # round-trip asserts arrival at all, which passed while TX died about a second in. Drives
    # the server's RX fan-out with 16384-byte frames (na_hamlib_bridge's rx_thread size, and
    # == AudioPacket::MAX_PAYLOAD) because the trigger was an unsendable RX datagram killing
    # the session, not anything on the TX path. Public C ABI only; hardware-free.
    add_executable(naudio_c_tx_sustained tests/c_tx_sustained.c)
    target_link_libraries(naudio_c_tx_sustained PRIVATE naudio)
    set_target_properties(naudio_c_tx_sustained PROPERTIES LINKER_LANGUAGE CXX)
    add_test(NAME naudio_c_tx_sustained COMMAND naudio_c_tx_sustained)

    # Client reliability profile: na_client_set_reliability_profile is the only way a C-ABI client
    # can turn on FEC / reorder / adaptive jitter / control-ARQ (na_client_set_transport writes the
    # transport and nothing else). Asserts the argument contract, that the PROFILE ALONE selects the
    # transport — the client never calls na_client_set_transport and still reaches a UDP-only server —
    # that audio flows with the whole reliability pipeline in place, and that last-writer-wins holds
    # against both a later set_transport and a later NA_RELIABILITY_DEFAULT. Loopback is loss-free, so
    # this CANNOT distinguish FEC-on from FEC-off; that needs induced loss. Public C ABI only.
    add_executable(naudio_c_client_profile tests/c_client_profile.c)
    target_link_libraries(naudio_c_client_profile PRIVATE naudio)
    set_target_properties(naudio_c_client_profile PROPERTIES LINKER_LANGUAGE CXX)
    add_test(NAME naudio_c_client_profile COMMAND naudio_c_client_profile)

    # Client reliability counters: na_client_get_stats is the observability half of
    # na_client_set_reliability_profile — the profile turns FEC / reorder / jitter ON and this is how
    # a C consumer sees them work. Asserts the argument contract, that an unconnected client reports
    # connected == 0 with the unmeasured gap counters at -1 (never 0, which would read as "no loss"),
    # that TCP leaves every reliability counter off, and — the headline — that
    # packets_recovered_by_fec MOVES under real induced loss and stays 0 on the same relay with
    # nothing dropped. client_stats_proxy.cpp is a C++ fixture (extern "C") that relays UDP between
    # client and server and discards one audio packet per FEC block; the C side has no portable
    # socket layer of its own, and naudio::net::Socket already is one. Links naudio (the public C
    # ABI) + naudio_net (the fixture's Socket) + Threads.
    add_executable(naudio_c_client_stats tests/c_client_stats.c tests/client_stats_proxy.cpp)
    target_link_libraries(naudio_c_client_stats PRIVATE naudio naudio_net Threads::Threads)
    set_target_properties(naudio_c_client_stats PROPERTIES LINKER_LANGUAGE CXX)
    add_test(NAME naudio_c_client_stats COMMAND naudio_c_client_stats)

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
    #
    # Not under TSan either, for the SAME reason one layer down: the ThreadSanitizer runtime
    # supplies its own operator new, which wins over this TU's definition, so g_failAlloc stops
    # having any effect and the arm fails with "A1 setter under OOM: ret=0 ... (want NA_ERR_NOMEM)"
    # — a report about the instrument, not about naudio. MEASURED per sanitizer rather than assumed
    # for all of them: the same arm PASSES under address,undefined (the ASan runtime does not
    # displace the override), so the exclusion is deliberately narrowed to thread and ASan keeps
    # its OOM coverage. Excluded at REGISTRATION, matching how WIN32 is handled above, so the arm
    # never reports a pass it did not earn.
    if(NOT WIN32 AND NOT "thread" IN_LIST _na_san_list)
        add_executable(naudio_c_abi_fault tests/c_abi_fault.cpp)
        target_link_libraries(naudio_c_abi_fault PRIVATE naudio)
        add_test(NAME naudio_c_abi_fault COMMAND naudio_c_abi_fault)
    endif()

    # Bridge RX probe: a content-aware naudio client, split deliberately from the thing it probes.
    # Six sessions hand-built a na_hamlib_bridge harness in a scratchpad and all six evaporated
    # (issue #15); nearly all of every one of them was this probe, and none of it needed libhamlib.
    # So the probe is built HERE, unconditionally, against the public C ABI alone — it compiles and
    # runs on the whole CI matrix — while the arm that drives the real bridge is registered after
    # add_subdirectory(tools) and exists only when that opt-in target does.
    #
    # --selftest is what stops "built in CI" from meaning "compiled but never executed": it stands
    # up an in-process NULL-backend server, injects known content and then silence, and asserts the
    # probe's detector separates them. That negative control is the whole reason the probe measures
    # peak |sample| instead of byte counts — measured against the Hamlib dummy, a silent bridge and
    # a healthy one differ by 0.2% in bytes and by 16383 in peak.
    add_executable(naudio_bridge_probe tests/bridge/na_bridge_probe.c)
    target_link_libraries(naudio_bridge_probe PRIVATE naudio)
    set_target_properties(naudio_bridge_probe PROPERTIES LINKER_LANGUAGE CXX)
    add_test(NAME naudio_bridge_probe_selftest COMMAND naudio_bridge_probe --selftest)

    # On-air soak analyser (issue #12 item 3) — the same discipline one level up. #12 asks for
    # "no unbounded drift in overruns/underruns over a multi-hour run", which is a claim about a
    # TREND in a log nobody reads line by line, so it needs an instrument rather than an eyeball.
    # The instrument will spend its whole life reporting "no drift", which is exactly the shape
    # CLAUDE.md L222 warns about: a detector that has never been shown to fire is indistinguishable
    # from a broken one. --selftest is the positive control — it drives synthetic flat, sustained,
    # timestamped and truncated logs through the real code path and asserts all four verdicts,
    # including that a truncated soak is a FAULT and not a pass, and that the counter which did not
    # move is not flagged.
    #
    # Registered unconditionally on POSIX: it parses text and needs no libhamlib, no PortAudio and
    # no radio, so it is verified on every build long before any hardware is attached. NOT WIN32
    # only because it is bash. The analyser it gates is run BY HAND during the on-air session —
    # see docs/hamlib-streaming-bridge.md § "On-air verification".
    if(NOT WIN32)
        add_test(NAME naudio_health_drift
                 COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/bridge/health_drift.sh" --selftest)
    endif()

    # Documentation honesty gate: extract every ```c / ```cpp
    # fenced block from README.md and compile each (-fsyntax-only) against the
    # in-tree headers, so a documented example that stops matching the real C ABI /
    # C++ API fails CI. POSIX-only (a bash + cc/c++ harness); the headers it checks
    # are platform-neutral, so running it on the Linux + macOS matrix covers them.
    #
    # docs/hamlib-streaming-bridge.md is checked too: its C blocks call the client
    # ABI (na_client_set_reliability_profile, na_client_get_stats), so an ABI change
    # can make them stale exactly as it can a README block. Its snippets are
    # fragments of a client the prose already built, so their free identifiers come
    # from tests/readme-snippets/preamble.hamlib-streaming-bridge.md.c — a per-FILE
    # preamble, kept out of the shared one so README's blocks stay as tightly
    # checked as they were. Any further doc added here needs the same treatment;
    # check.sh documents the sidecar naming.
    if(NOT WIN32)
        add_test(NAME naudio_readme_snippets
                 COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/readme-snippets/check.sh"
                              "${CMAKE_CURRENT_SOURCE_DIR}/README.md"
                              "${CMAKE_CURRENT_SOURCE_DIR}/docs/hamlib-streaming-bridge.md")
    endif()

    # C99 consumer gate: compile a translation unit that includes include/naudio.h at
    # -std=c99 -pedantic-errors. The comment at the top of this file and CONTRIBUTING.md both
    # promise a consumer that the header is C99-clean and that only the TESTS need C11 — and until
    # now nothing re-checked it, because every gate that compiles C against this header does so at
    # C11 or higher (tests/external-consumer/CMakeLists.txt, tests/ffi-clients/run.sh,
    # tests/readme-snippets/check.sh, and this build). A future _Static_assert or _Alignas in the
    # header would compile clean in all four, silently raise the floor for every C99 consumer, and
    # leave both of those sentences false in the shipped tree. Issue #45.
    #
    # This is #42's shape one layer out, with the difference that #42 was at least LOUD on MSVC:
    # the C99 consumer who would notice this one is not in the repo, so the gate has to stand in
    # for them. check.cmake carries its own positive control for the same reason — see its header.
    #
    # Compiler-gated rather than platform-gated, and the distinction is the point: -pedantic-errors
    # has no MSVC equivalent (MSVC has no C99 mode at all), so the skip is about the FRONT END, not
    # about Windows. A MinGW or MSYS toolchain runs this gate perfectly well. clang-cl is excluded
    # too — it reports CMAKE_C_COMPILER_ID as Clang while driving an MSVC-style command line that
    # spells these flags differently. The header's constructs are platform-neutral, so the
    # Linux + macOS matrix covers them; the skip costs no coverage. It is announced rather than
    # silent because a gate that fails to register reports the same green as one that passed.
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang"
       AND NOT CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
        add_test(NAME naudio_c99_header_gate
                 COMMAND "${CMAKE_COMMAND}"
                         "-DNA_CC=${CMAKE_C_COMPILER}"
                         "-DNA_INCLUDE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/include"
                         "-DNA_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/tests/c99-header-gate"
                         "-DNA_WORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/c99-header-gate"
                         -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/c99-header-gate/check.cmake")
        file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/c99-header-gate")
    else()
        message(STATUS "naudio: C99 header gate SKIPPED — ${CMAKE_C_COMPILER_ID} has no "
                       "-pedantic-errors equivalent (issue #45; Linux/macOS cover the header)")
    endif()

    # Spec-citation gate: every `docs/audio-streaming-protocol-v1.md:NNN` in the tree must still
    # resolve to a line carrying its registered anchor text. Issue #88 item 7 (from #78).
    #
    # These citations live in COMMENTS and one string literal, so nothing compiles them and nothing
    # failed when they rotted. #73 broke seven at once — a single paragraph added to §2.2 shifted
    # every line below it — and the suite passed 369/369 before and after. They were repaired by
    # hand, twice (1313d0e, 06fa36a), which is the actual problem: the repair is a grep a human has
    # to remember to run, and the grep cannot see two of the three citation shapes.
    #
    # That is not hypothetical either. Writing this gate found a citation the hand pass had missed
    # for four days: src/net/AudioMixer.cpp's `(spec :325)` still read `:323`, correct when it was
    # written and shifted +2 by that same #73 paragraph. No path-anchored grep can find it — it
    # names no file — so it survived both repair commits and is fixed in the commit that adds this.
    #
    # Runs everywhere, unlike the two doc gates above: it is pure CMake text processing with no
    # compiler and no shell, so there is no front end to gate on and no reason to skip Windows.
    add_test(NAME naudio_spec_citations
             COMMAND "${CMAKE_COMMAND}"
                     "-DNA_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
                     "-DNA_WORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/spec-citations"
                     -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/spec-citations/check.cmake")
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/spec-citations")

    # Packaged-documentation gate: performs a real `cmake --install` into a scratch prefix and
    # asserts the licence text, the wire spec and the conformance vectors came out the other side,
    # byte-identical to their sources. It carries its own negative control (see check.cmake).
    #
    # This exists because on 2026-08-31 an audit measured all of them ABSENT from every one of the
    # six shipped artifact forms, while being present in the repo the whole time and while
    # CPACK_RESOURCE_FILE_LICENSE was set and appeared to cover the licence. No source-tree grep
    # and no configuration read could have caught that — what a package contains is a property of
    # the artifact. So the gate installs and looks.
    #
    # Runs everywhere: `cmake --install` and `compare_files` are portable, and the paths are built
    # from CMAKE_INSTALL_DATAROOTDIR rather than a hard-coded "share".
    add_test(NAME naudio_packaged_docs
             COMMAND "${CMAKE_COMMAND}"
                     "-DNA_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
                     "-DNA_BUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}"
                     "-DNA_WORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/packaged-docs"
                     "-DNA_DATAROOTDIR=${CMAKE_INSTALL_DATAROOTDIR}"
                     # $<CONFIG> resolves at test time, which is the only way this works on a
                     # MULTI-CONFIG generator: Visual Studio puts binaries in build/<Config>/,
                     # and `cmake --install` with no --config silently assumes Release. The
                     # Windows CI job builds RelWithDebInfo, so the install went looking in
                     # build/Release/ and failed on a naudio.dll that was never there.
                     "-DNA_CONFIG=$<CONFIG>"
                     -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/packaged-docs/check.cmake")
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/packaged-docs")

    # Listener-lifetime gate: every addStreamListener() CALL in the tree must be registered in
    # tests/listener-lifetime/sites.txt with the mechanism that keeps its borrowed listener alive.
    # Issue #97.
    #
    # The contract is that a listener outlives its owner unless it is handed back with
    # removeStreamListener(), and breaking it is a use-after-free the compiler cannot see, the
    # suite passes over, and only a sanitizer catches — intermittently, because it is a race
    # between the owner's destructor draining the dispatcher and the listener leaving scope. It
    # has now been broken three times: #28 found two arms in test_server.cpp, #97 found a third
    # there after fourteen days of green CI, and the sweep #97 asked for found fourteen more in
    # test_client_e2e.cpp plus one in the SHIPPED na_audio_daemon. The countermeasure both earlier
    # rounds shipped was a header comment, and this is what a header comment is worth.
    #
    # It does NOT read declaration order — that needs a C++ front end. It closes the recurrence
    # path that has actually recurred every time: a NEW call site written without the contract in
    # mind. The gate's own limits are stated at the top of check.cmake rather than left implied.
    #
    # Runs everywhere, like the spec-citation gate above and for the same reason: pure CMake text
    # processing, no compiler and no shell.
    add_test(NAME naudio_listener_lifetime
             COMMAND "${CMAKE_COMMAND}"
                     "-DNA_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
                     -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/listener-lifetime/check.cmake")
endif()

# ----------------------------------------------------------------------------
# tools/ — the exercisers. Deliberately OUTSIDE naudio_core/naudio: they drive the real
# AudioStreamServer / AudioStreamClient rather than being audio primitives, which keeps the core
# pure + portable. Added last so they can reuse the GoogleTest targets configured by the tests
# block.
#
# The WIN32 exclusion used to sit HERE, on the whole directory, and its stated reason was entirely
# about na_audio_daemon — "the daemon is a hardware smoke CI never runs". That reason does not
# generalise to the directory: na_wav_tap opens no device at all (NA_CLIENT_BACKEND_NULL) and is
# most useful on Windows precisely because issue #12 item 1 wants a REMOTE client, which is
# whatever machine the operator has spare. So the gate moved INWARD, per target, in tools/. The
# Windows-excluded set is unchanged — the daemon and the Hamlib bridge are still guarded there —
# this only stops a device-free client tool from being excluded by association.
# ----------------------------------------------------------------------------
add_subdirectory(tools)

# ----------------------------------------------------------------------------
# Daemon argument gate (issue #63) — na_audio_daemon's main() has no injection seam, so driving
# the built binary is the only coverage its option parsing can have. Every case the script runs is
# decided BEFORE any device work (the parse loop exits on a bad value; --transport is validated
# immediately after it), so this arm opens no PortAudio stream and is safe on a headless runner —
# unlike naudio_bridge_arm above, it needs no external dependency at all.
#
# Registered only where the target exists (NAUDIO_BUILD_APPS), so on a lib-only configuration
# ctest has no such test rather than a skip that reads like coverage. That is the same discipline
# as the bridge arm, and it is why per-job ctest totals differ by design.
#
# Windows is no longer excluded (issue #96 item 3). Both gates drive the built binary and assert
# on its output, which is platform-neutral — what is NOT neutral is the bash they are written in.
# Every POSIX host has one; a Windows host has one only if Git for Windows is installed (it ships
# Git Bash, and it is on PATH on the GitHub runners). So the shell is REQUIRED TO EXIST before the
# arms are registered, rather than registered unconditionally and left to fail as "unable to find
# executable" on a developer's machine — a failure that would read like a defect in the daemon
# instead of a missing tool. Same rule as everywhere else here: no test beats a misleading one.
# ----------------------------------------------------------------------------
set(NAUDIO_DAEMON_GATE_SHELL bash)
if(WIN32)
    find_program(NAUDIO_GATE_BASH NAMES bash)
    if(NAUDIO_GATE_BASH)
        set(NAUDIO_DAEMON_GATE_SHELL "${NAUDIO_GATE_BASH}")
    else()
        unset(NAUDIO_DAEMON_GATE_SHELL)
        message(STATUS "naudio: na_audio_daemon's argcheck/configcheck gates need a bash; none "
                       "found on this Windows host, so they are not registered (install Git for "
                       "Windows to get them)")
    endif()
endif()
if(NAUDIO_BUILD_TESTS AND NAUDIO_DAEMON_GATE_SHELL AND TARGET na_audio_daemon)
    add_test(NAME naudio_daemon_argcheck
             COMMAND "${NAUDIO_DAEMON_GATE_SHELL}"
                          "${CMAKE_CURRENT_SOURCE_DIR}/tests/daemon/argcheck.sh"
                          "$<TARGET_FILE:na_audio_daemon>")
    # Config-file gate (issue #96 item 1) — same containment as argcheck: every case is decided
    # before any device work, so the arm is hardware-free despite driving the hardware tool. On
    # Windows this is also the ONLY execution anywhere of defaultConfigPath()'s _WIN32 branch:
    # %ProgramData% is an environment variable, so unlike Linux's absolute /etc/naudio the default
    # search can be redirected into a scratch tree and actually tested.
    add_test(NAME naudio_daemon_configcheck
             COMMAND "${NAUDIO_DAEMON_GATE_SHELL}"
                          "${CMAKE_CURRENT_SOURCE_DIR}/tests/daemon/configcheck.sh"
                          "$<TARGET_FILE:na_audio_daemon>")
    # Service-unit gate (issue #96 item 2). A launchd plist and a systemd unit are code nothing
    # compiles: a typo'd option ships, installs, and fails only on the operator's machine. This
    # arm extracts the generated unit's OWN argument list and runs it against the built binary,
    # and asserts the absolute path it names is the path this build installs to — the failure a
    # build-tree test structurally cannot see. Registered only where the unit was generated, so
    # a configuration with no daemon has no such test rather than a skip that reads like
    # coverage; the platform decides WHICH unit, so ubuntu CI covers the systemd arm and macOS
    # the launchd one. The fourth argument is the generated control app Info.plist, set on
    # macOS only: the launchd arm asserts the agent's AssociatedBundleIdentifiers is that
    # bundle's CFBundleIdentifier and that the bundle carries one name (issue #102), and treats
    # the argument's absence as a harness fault rather than a skip. The fifth is the HTTP
    # client the arm reads the pipeline state with when it RUNS the unit's arguments (issue
    # #102: inertness is a behaviour of those arguments now, not a Disabled key) — located
    # here, shared with the control-page gate below, and absent-as-harness-fault for the same
    # reason as the fourth. It binds a socket and polls for the daemon's first line, so it gets
    # the control-page gate's timeout.
    find_program(NAUDIO_GATE_CURL NAMES curl)
    if(NAUDIO_SERVICE_UNIT_FILE)
        add_test(NAME naudio_daemon_servicecheck
                 COMMAND "${NAUDIO_DAEMON_GATE_SHELL}"
                              "${CMAKE_CURRENT_SOURCE_DIR}/tests/daemon/servicecheck.sh"
                              "$<TARGET_FILE:na_audio_daemon>"
                              "${NAUDIO_SERVICE_UNIT_FILE}"
                              "${NAUDIO_SERVICE_EXEC_PATH}"
                              "${NAUDIO_APP_INFO_PLIST}"
                              "${NAUDIO_GATE_CURL}")
        set_tests_properties(naudio_daemon_servicecheck PROPERTIES TIMEOUT 180)
    endif()
    # Control-page gate (issue #96 item 4). The page is the surface an operator reaches INSTEAD of
    # a terminal, so its failures are the ones with no exit code anywhere — a browser shows a
    # blank panel and nothing else happens. The arm drives the real HTTP server in the built
    # binary: it asserts the page and its script are served, that the three browser defences (Host,
    # Origin, Content-Type) each refuse WITH a positive control proving the refusal was for that
    # reason, that a bad value is refused with the same message the flag parser gives and leaves
    # the config file untouched, and that a valid write round-trips through the file and back.
    #
    # Hardware-free by construction, not by luck: control mode opens no device until something
    # asks it to, the arm passes --autostart false and asserts the pipeline is idle, and it never
    # posts a start.
    #
    # Two external tools, both LOCATED BY CMAKE and passed in rather than assumed by the script:
    # an HTTP client (found above, for the service-unit gate), and a Python to read JSON out of
    # the responses (a regex over a message that can contain a quote or a comma is how a harness
    # starts lying). Same discipline as the bash probe above — where either is missing the arm is
    # not registered at all, because a test that fails as "unable to find executable" reads like
    # a defect in the daemon.
    find_program(NAUDIO_GATE_PYTHON NAMES python3 python)
    if(NAUDIO_GATE_CURL AND NAUDIO_GATE_PYTHON)
        add_test(NAME naudio_daemon_controlcheck
                 COMMAND "${NAUDIO_DAEMON_GATE_SHELL}"
                              "${CMAKE_CURRENT_SOURCE_DIR}/tests/daemon/controlcheck.sh"
                              "$<TARGET_FILE:na_audio_daemon>"
                              "${NAUDIO_GATE_PYTHON}"
                              "${NAUDIO_GATE_CURL}")
        # It binds a socket and polls for the daemon's first line; the default 1500 s is far more
        # than it needs, but a hung daemon should fail the arm rather than the whole suite.
        set_tests_properties(naudio_daemon_controlcheck PROPERTIES TIMEOUT 180)
    else()
        message(STATUS "naudio: the control-page gate needs curl and python3; one is missing, so "
                       "naudio_daemon_controlcheck is not registered")
    endif()
endif()

# ----------------------------------------------------------------------------
# Bridge arm (issue #15) — the half of the bridge harness that cannot be portable. Registered ONLY
# when na_hamlib_bridge actually exists as a target, which needs both NAUDIO_BUILD_HAMLIB_BRIDGE=ON
# and a streaming-capable libhamlib; on a stock build ctest has no such test at all, rather than a
# skip that reads like coverage. Must come after add_subdirectory(tools), where that target is
# defined. The detector this arm leans on is proved unconditionally and on every platform by
# naudio_bridge_probe --selftest above, so the portable half never depends on this one running.
# ----------------------------------------------------------------------------
if(NAUDIO_BUILD_TESTS AND NOT WIN32 AND TARGET na_hamlib_bridge AND TARGET naudio_bridge_probe)
    add_test(NAME naudio_bridge_arm
             COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/bridge/bridge_arm.sh"
                          "$<TARGET_FILE:na_hamlib_bridge>"
                          "$<TARGET_FILE:naudio_bridge_probe>")
    message(STATUS "naudio: naudio_bridge_arm enabled (na_hamlib_bridge present)")

    # Fault-injection arm (issue #35) — the same gate, one target further. naudio_failshim is a
    # PRELOAD library, never linked by anything: it is inserted into the bridge's process at launch
    # and interposes libhamlib's two stream calls, which is the only way to reach the error paths
    # issues #5 and #6 describe (the Hamlib dummy has no error-injection knob, and it routes reads
    # through its own caps->stream_read hook).
    #
    # NOT registered against a static-only hamlib prefix (build-streaming-hamlib.sh --static, the
    # release-packaging shape — issue #93): rig_stream_read/write are then linked INTO
    # na_hamlib_bridge as direct calls, so there is no dynamic symbol for DYLD_INSERT_LIBRARIES /
    # LD_PRELOAD to bind and the arm can only fail as a harness fault — which is exactly what it
    # did (measured: "the shim loaded but rig_stream_read was never interposed", 478/479). Skipped
    # at REGISTRATION with a STATUS naming the reason, matching how WIN32 and TSan exclusions are
    # handled above, so the release suite is honestly green rather than structurally red — and the
    # CI hamlib-bridge job, which builds the SHARED prefix, still asserts this arm BY NAME on every
    # run, so the coverage cannot silently vanish from the matrix.
    if(NAUDIO_HAMLIB_STATIC_ONLY)
        message(STATUS "naudio: naudio_bridge_fault_arm NOT registered — static libhamlib: the "
                       "preload shim has no dynamic rig_stream_* to interpose. Fault coverage "
                       "needs the default shared-libhamlib prefix.")
    else()
    #
    # SHARED, and NOT MODULE, even though nothing links it. MODULE builds a -bundle on macOS, and
    # DYLD_INSERT_LIBRARIES flatly refuses one: `cannot link against bundle`, at which point dyld
    # aborts the process before main. Measured — the arms failed as harness faults, not as bridge
    # bugs, which is the tri-state working. MODULE is the intuitively correct choice here and it is
    # wrong on macOS.
    #
    # The HAMLIB_* variables come from pkg_check_modules inside tools/, which caches them, so they
    # are visible here; HAMLIB_LINK_LIBRARIES is NOT cached and deliberately not used.
    add_library(naudio_failshim SHARED tests/bridge/failshim.c)
    target_include_directories(naudio_failshim PRIVATE ${HAMLIB_INCLUDE_DIRS})
    target_link_directories(naudio_failshim PRIVATE ${HAMLIB_LIBRARY_DIRS})
    target_link_libraries(naudio_failshim PRIVATE ${HAMLIB_LIBRARIES})
    # No "lib" prefix and a platform-native suffix: the driver passes this path straight to
    # DYLD_INSERT_LIBRARIES / LD_PRELOAD, which want the file, not a linker name.
    #
    # C_VISIBILITY_PRESET default overrides this project's global `hidden` (CMAKE_C_VISIBILITY_PRESET
    # above) for this target alone. Hidden is right for a shipped library and fatal for a preload one:
    # on ELF it leaves the two interposers as LOCAL symbols — `nm` shows `t` rather than `T` and
    # `nm -D` shows nothing at all — so LD_PRELOAD has nothing to bind and both functions are dead
    # code. Measured on Ubuntu 24.04 / gcc 13.3 before this line existed: the shim still loaded and
    # still printed its `armed` marker, so the fault arm failed as though the BRIDGE had regressed and
    # reported issues #5 and #6 as live bugs. macOS never showed it, because __DATA,__interpose binds
    # the local definition directly and does not consult the dynamic symbol table.
    #
    # Deleting this line no longer produces that misdiagnosis: the arms also require the shim's
    # `interposed <fn>` marker, printed from inside the interposed call, so an unbound interposer is
    # now reported as a harness fault instead (issue #39, tests/bridge/failshim.c). That is a better
    # error message and not a reason to drop the property — the arms would still all fail.
    set_target_properties(naudio_failshim PROPERTIES PREFIX "" C_VISIBILITY_PRESET default)

    add_test(NAME naudio_bridge_fault_arm
             COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/bridge/bridge_fault_arm.sh"
                          "$<TARGET_FILE:na_hamlib_bridge>"
                          "$<TARGET_FILE:naudio_bridge_probe>"
                          "$<TARGET_FILE:naudio_failshim>")
    message(STATUS "naudio: naudio_bridge_fault_arm enabled (na_hamlib_bridge present)")
    endif()

    # netrigctl arm (issue #88 item 8) — the ONLY arm that drives `-m 2`. Every other bridge arm
    # runs the dummy backend in-process, so the `\stream_open` round trip over a wire had no
    # automated coverage at all; #21 sat open for a month after its cause was gone because of it.
    #
    # NO_DEFAULT_PATH IS LOAD-BEARING, NOT TIDINESS. The arm must run the rigctld belonging to the
    # SAME prefix as the libhamlib na_hamlib_bridge links, and a search that falls through to the
    # system PATH finds a distro 4.x rigctld with no streaming subsystem whatsoever — the arm would
    # then fail at `\stream_caps` and read as a naudio regression. Worse is the near miss: a second
    # hand-built prefix on PATH gives a version-skewed peer, and a bridge older than 961093f2
    # against a newer rigctld does not degrade, it refuses to open the stream (see the harness's
    # own diagnosis for that case). Pinning the search to HAMLIB_PREFIX makes the peer's version a
    # property of the prefix the operator chose, which is the only version relationship the bridge
    # documents.
    #
    # Registered only if it is actually found, and announced when it is not, for the same reason
    # the arms above are: a gate that silently fails to register reports the same green as one that
    # passed (Learning 9's shape in the test registry rather than in the build).
    find_program(NAUDIO_RIGCTLD rigctld HINTS "${HAMLIB_PREFIX}/bin" NO_DEFAULT_PATH)
    if(NAUDIO_RIGCTLD)
        add_test(NAME naudio_netrigctl_arm
                 COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/bridge/netrigctl_arm.sh"
                              "$<TARGET_FILE:na_hamlib_bridge>"
                              "${NAUDIO_RIGCTLD}"
                              "$<TARGET_FILE:naudio_bridge_probe>")
        message(STATUS "naudio: naudio_netrigctl_arm enabled (rigctld ${NAUDIO_RIGCTLD})")
    else()
        message(STATUS "naudio: naudio_netrigctl_arm SKIPPED — no rigctld in ${HAMLIB_PREFIX}/bin, "
                       "so the -m 2 path has no peer to talk to (issue #88 item 8)")
    endif()

    # The bridge arms bind fixed ports and all drive a dummy backend, so they are serialised rather
    # than left to race a port bind under `ctest -j`. The netrigctl arm binds a second (TCP) port of
    # its own for rigctld and is held under the same lock for the same reason.
    set(NAUDIO_BRIDGE_ARMS naudio_bridge_arm)
    if(NOT NAUDIO_HAMLIB_STATIC_ONLY)
        list(APPEND NAUDIO_BRIDGE_ARMS naudio_bridge_fault_arm)
    endif()
    if(NAUDIO_RIGCTLD)
        list(APPEND NAUDIO_BRIDGE_ARMS naudio_netrigctl_arm)
    endif()
    set_tests_properties(${NAUDIO_BRIDGE_ARMS} PROPERTIES RESOURCE_LOCK bridge)

    # OUTER backstop only (issue #88 item 2, from #41). The real deadlines live inside the harnesses
    # — tests/bridge/deadline.sh — and that is deliberately where the work went: a harness deadline
    # FAILS the arm with a diagnosis naming which probe or which bridge wedged, while this property
    # only kills the job and prints a timeout. Both arms run in well under a minute on a healthy
    # machine, so 600 s is far above any legitimate run and exists purely to bound a hang the
    # harness itself somehow failed to catch. Note ctest's DEFAULT is 1500 s, which is what let #41
    # sit for ~29 minutes before anyone intervened.
    set_tests_properties(${NAUDIO_BRIDGE_ARMS} PROPERTIES TIMEOUT 600)
    unset(NAUDIO_BRIDGE_ARMS)
endif()

# ----------------------------------------------------------------------------
# examples/ — reference consumers of the PUBLIC na_* C ABI (the play-to-speakers
# clients in C/C++/Python/Java/Rust and the demo source). These link ONLY the public
# shared `naudio` and include only naudio.h — the proof the C ABI is self-sufficient
# from pure C. Added after the `naudio` shared target is defined; gated internally by
# NAUDIO_BUILD_APPS.
# ----------------------------------------------------------------------------
add_subdirectory(examples)

# na_audio_source --playback-id refusal gate. Same containment as the daemon's argcheck: every
# case is decided before any device work (the --test-tone pairing is refused straight after the
# parse loop's range checks), so the arm is hardware-free although it drives the SYSTEM-backend
# example. The one case that starts anything is the positive control, a one-second test tone on
# an ephemeral port, on the NULL backend. Registered only where the target and a bash exist, for
# the reasons given above the daemon gates: no test beats a misleading one. It sits AFTER
# add_subdirectory(examples) because that is where the target comes into existence — placed
# beside the daemon gates it silently registered nothing (the TARGET test was false there).
if(NAUDIO_BUILD_TESTS AND NAUDIO_DAEMON_GATE_SHELL AND TARGET na_audio_source)
    # The trailing argument is the shared library's directory: on Windows the examples land in
    # build/examples/<Config>/ and naudio.dll beside the library target, so without it both arms
    # exit 127 from ctest ("naudio.dll: cannot open shared object file") — seen on the first CI run
    # of this arm. The script prepends it to PATH (through cygpath, the exe being native).
    add_test(NAME naudio_source_argcheck
             COMMAND "${NAUDIO_DAEMON_GATE_SHELL}"
                          "${CMAKE_CURRENT_SOURCE_DIR}/tests/examples/source_argcheck.sh"
                          "$<TARGET_FILE:na_audio_source>"
                          "$<TARGET_FILE_DIR:naudio>")
endif()
# The TX-owner line, end to end and hardware-free: the test-tone server on the NULL backend, the
# tone client transmitting from the NULL backend, and the assertion on the LOG LINE a station reads
# its PTT-to-audio latency from — present and stamped while a client's audio is mixed, "none" once
# released, absent for a client that connects and sends nothing (the negative control). Two
# servers of eight and four seconds on ephemeral ports; the timeout is for a hung one.
if(NAUDIO_BUILD_TESTS AND NAUDIO_DAEMON_GATE_SHELL AND TARGET na_audio_source AND TARGET na_c_inject_tone)
    add_test(NAME naudio_tx_owner_line
             COMMAND "${NAUDIO_DAEMON_GATE_SHELL}"
                          "${CMAKE_CURRENT_SOURCE_DIR}/tests/examples/tx_owner_line.sh"
                          "$<TARGET_FILE:na_audio_source>"
                          "$<TARGET_FILE:na_c_inject_tone>"
                          "$<TARGET_FILE_DIR:naudio>")
    # The servers now live until the client is done (a 60 s ceiling each); the arm's own budget
    # covers two of them plus the release poll with room, and a hung one still fails the arm
    # rather than the suite.
    set_tests_properties(naudio_tx_owner_line PROPERTIES TIMEOUT 150)
endif()

# ----------------------------------------------------------------------------
# CPack — binary packages (issue #93). TGZ is the default, relocatable shape on
# every platform (the installed tools self-locate libnaudio through the relative
# rpath set near the top of this file); the release workflow additionally asks
# for DEB + RPM on Linux and ZIP on Windows via `cpack -G`. A release-shaped
# tree configures with NAUDIO_BUNDLE_PORTAUDIO=ON and (POSIX) a --static
# streaming-hamlib prefix, so the packaged binaries carry no dependency a
# package manager cannot satisfy — the release workflow asserts that on the
# artifacts themselves, never on this configuration having been used.
#
# Top-level only: a parent project embedding naudio via FetchContent/
# add_subdirectory must not inherit naudio's packaging identity.
# ----------------------------------------------------------------------------
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(CPACK_PACKAGE_NAME "naudio")
    set(CPACK_PACKAGE_VENDOR "KJ5HST")
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
        "net-audio streaming toolkit: C ABI, multi-tenant server/client, reliability stack")
    set(CPACK_PACKAGE_DESCRIPTION "${PROJECT_DESCRIPTION}")
    set(CPACK_PACKAGE_CONTACT "KJ5HST <kj5hst@deppe.com>")
    set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/KJ5HST-LABS/naudio")
    set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
    set(CPACK_RESOURCE_FILE_README "${CMAKE_CURRENT_SOURCE_DIR}/README.md")
    if(NOT WIN32)
        # MSVC keeps symbols in PDBs and ships no strip tool; POSIX release
        # binaries are stripped at packaging time.
        set(CPACK_STRIP_FILES ON)
    endif()

    # naudio-<version>-<os>-<arch>: lowercase, human-first names (Darwin is
    # "macos" to the people downloading it), uniform across the three
    # per-platform CPack generators so the release page sorts as one family.
    string(TOLOWER "${CMAKE_SYSTEM_NAME}" NAUDIO_PKG_OS)
    if(NAUDIO_PKG_OS STREQUAL "darwin")
        set(NAUDIO_PKG_OS "macos")
    endif()
    string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" NAUDIO_PKG_ARCH)
    if(NAUDIO_PKG_ARCH STREQUAL "amd64")
        set(NAUDIO_PKG_ARCH "x86_64")
    endif()
    # Pre-release qualifier (e.g. "rc1"). project(VERSION) is numeric-only by
    # CMake's own grammar, so a release candidate cannot live there — it rides
    # the TAG (v1.0.0rc1) and this variable, which the release workflow derives
    # FROM the tag and passes at configure time. Empty for final releases and
    # for every ordinary build. It shapes the artifact file names (so rc and
    # final artifacts cannot collide on a release page) and the DEB/RPM
    # internal versions, where the "~" makes the package manager sort the rc
    # BEFORE the final release it precedes (dpkg and rpm both order tilde
    # first), so upgrading rc -> final works instead of comparing equal.
    set(NAUDIO_PKG_VERSION_SUFFIX "" CACHE STRING
        "Pre-release qualifier appended to package names/versions (e.g. rc1)")
    set(CPACK_PACKAGE_FILE_NAME
        "naudio-${PROJECT_VERSION}${NAUDIO_PKG_VERSION_SUFFIX}-${NAUDIO_PKG_OS}-${NAUDIO_PKG_ARCH}")
    set(CPACK_GENERATOR "TGZ")
    if(NAUDIO_PKG_VERSION_SUFFIX)
        set(CPACK_DEBIAN_PACKAGE_VERSION "${PROJECT_VERSION}~${NAUDIO_PKG_VERSION_SUFFIX}")
        set(CPACK_RPM_PACKAGE_VERSION "${PROJECT_VERSION}~${NAUDIO_PKG_VERSION_SUFFIX}")
    endif()

    # DEB/RPM install under /usr (the generators' default packaging prefix), so
    # the Linux release job configures with CMAKE_INSTALL_PREFIX=/usr and the
    # installed naudio.pc / naudioConfig.cmake are truthful after `dpkg -i`.
    # SHLIBDEPS computes Depends from what the binaries actually link (libasound2
    # etc. — measured, not hand-maintained); PRIVATE_DIRS keeps dpkg-shlibdeps
    # from failing on the package's OWN libnaudio, which no installed package
    # provides at build time.
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS_PRIVATE_DIRS "${CMAKE_INSTALL_LIBDIR}")
    # "sound", not "hamradio": naudio is a general network-audio library with
    # amateur-radio uses, not a ham-radio package with an audio side.
    set(CPACK_DEBIAN_PACKAGE_SECTION "sound")
    set(CPACK_DEBIAN_FILE_NAME "${CPACK_PACKAGE_FILE_NAME}.deb")
    set(CPACK_RPM_PACKAGE_LICENSE "LGPL-2.1-or-later")
    set(CPACK_RPM_PACKAGE_GROUP "Applications/Communications")
    set(CPACK_RPM_FILE_NAME "${CPACK_PACKAGE_FILE_NAME}.rpm")

    # Double-clickable installers: macOS .pkg (productbuild) and Windows setup
    # .exe (NSIS), produced by the release workflow alongside the archives.
    # Their payload is scoped to runtime+tools at cpack time — see the project
    # config file below; a configure-time set() cannot vary by generator.
    set(CPACK_PROJECT_CONFIG_FILE "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CPackProjectConfig.cmake")
    if(APPLE)
        # Receipt identity: `pkgutil --pkgs` lists org.kj5hst.naudio.* after
        # install — also how an operator removes the receipt if they hand-delete
        # the files (macOS packages have no uninstaller by design).
        set(CPACK_PRODUCTBUILD_IDENTIFIER "org.kj5hst.naudio")

        # --- Payload code signing -------------------------------------------------
        # Apple's notary validates every Mach-O INSIDE the .pkg, not just the wrapper
        # productsign covers. Signing cannot happen at build time here: CMAKE_INSTALL_RPATH
        # above makes install_name_tool rewrite the load commands and CPACK_STRIP_FILES
        # strips symbols, and either invalidates an earlier signature — silently, until the
        # notary rejects it (2ae8cbb). CPACK_PRE_BUILD_SCRIPTS runs after CPack installs and
        # strips into its staging tree and before the package is assembled, which is the one
        # window where a signature applies to the final bytes AND survives to the artifact.
        #
        # EMPTY BY DEFAULT, deliberately: with no identity nothing is signed and nothing
        # changes, so every build that does not ask for this is byte-for-byte unaffected.
        set(NAUDIO_CODESIGN_IDENTITY "" CACHE STRING
            "Developer ID Application identity for signing macOS payload binaries (empty = unsigned)")
        if(NAUDIO_CODESIGN_IDENTITY)
            # Carried as a CPACK_ variable so it reaches the script's separate cmake -P
            # context via CPackConfig.cmake; a plain variable would not survive.
            set(CPACK_NAUDIO_CODESIGN_IDENTITY "${NAUDIO_CODESIGN_IDENTITY}")
            set(CPACK_PRE_BUILD_SCRIPTS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CodesignPayload.cmake")
            message(STATUS "naudio: macOS payload will be signed as '${NAUDIO_CODESIGN_IDENTITY}'")
        endif()
        # productbuild's license pane accepts only .rtf/.html/.txt/.rtfd — the
        # repo's extension-less LICENSE is rejected outright (measured: "Bad
        # file extension specified"). Stage a .txt copy; the project config
        # file points productbuild at it, leaving every other generator on the
        # original files.
        configure_file(LICENSE "${CMAKE_CURRENT_BINARY_DIR}/cpack-resources/LICENSE.txt" COPYONLY)
        set(CPACK_NAUDIO_LICENSE_TXT "${CMAKE_CURRENT_BINARY_DIR}/cpack-resources/LICENSE.txt")
        set(CPACK_NAUDIO_PKG_README_TXT "${CMAKE_CURRENT_SOURCE_DIR}/cmake/productbuild-readme.txt")
        # Service registration rides the installer (issue #96 item 2): the payload
        # cannot place /Library/LaunchAgents/org.kj5hst.naudio.daemon.plist, because
        # every payload path is rooted at the /usr/local install prefix. A postinstall
        # script copies it out of share/naudio/ instead. Generated in tools/ (where the
        # install paths it bakes in are known) and handed to the productbuild branch of
        # the project config file; empty on a lib-only build, where there is no daemon
        # to register and CPack must not be given a script path that does not exist.
        if(NAUDIO_POSTFLIGHT_SCRIPT)
            set(CPACK_NAUDIO_POSTFLIGHT_SCRIPT "${NAUDIO_POSTFLIGHT_SCRIPT}")
        endif()
    endif()
    if(WIN32)
        # $PROGRAMFILES64 is an NSIS variable, literal to CMake: 64-bit
        # binaries must not default into the x86 Program Files.
        set(CPACK_NSIS_INSTALL_ROOT "$PROGRAMFILES64")
        # Stable, unversioned directory so upgrades replace instead of accrete.
        set(CPACK_PACKAGE_INSTALL_DIRECTORY "naudio")
        set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
        # The tools are console programs: offer PATH so they run from any
        # prompt — the one thing a CLI payload needs from a GUI installer.
        set(CPACK_NSIS_MODIFY_PATH ON)
        set(CPACK_NSIS_DISPLAY_NAME "naudio")
        set(CPACK_NSIS_PACKAGE_NAME "naudio")
        set(CPACK_NSIS_URL_INFO_ABOUT "${CPACK_PACKAGE_HOMEPAGE_URL}")
        set(CPACK_NSIS_CONTACT "${CPACK_PACKAGE_CONTACT}")

        # The Start-menu entry that opens the control page (issue #96 item 4) — the "shortcut"
        # the issue's acceptance names. It is the same command line the .desktop file and the
        # macOS .app use, and clicking it twice is safe: control mode finds an already-running
        # page on the configured port, opens that, and exits 0 rather than failing to bind.
        #
        # $SMPROGRAMS/$STARTMENU_FOLDER are NSIS variables, literal to CMake (as $PROGRAMFILES64
        # is above); the whole command is an NSIS single-quoted string so the inner double quotes
        # around space-bearing paths are literal.
        #
        # FOUR backslashes, not two, and the difference is not cosmetic. CPack writes every
        # CPACK_* variable into CPackConfig.cmake as `set(VAR "<value>")` with NO escaping, and
        # cpack then RE-PARSES that file. Two here makes the value hold one backslash, which
        # lands in CPackConfig.cmake as `\bin` — and cpack dies with `Invalid character escape
        # '\b'` and `CPack project name not specified`, an error naming neither this variable
        # nor NSIS. Four makes the value hold two, which survives the round trip as one, which
        # is what NSIS gets. Reproduced end to end before being believed: two fails and four
        # round-trips to exactly the single backslashes below.
        set(CPACK_NSIS_CREATE_ICONS_EXTRA
            "CreateShortCut '$SMPROGRAMS\\\\$STARTMENU_FOLDER\\\\naudio Control.lnk' '$INSTDIR\\\\bin\\\\na_audio_daemon.exe' '--mode control --duration-ms 0 --open-page'")
        set(CPACK_NSIS_DELETE_ICONS_EXTRA
            "Delete '$SMPROGRAMS\\\\$MUI_TEMP\\\\naudio Control.lnk'")

        # Service registration rides the installer here exactly as the .pkg postinstall carries
        # it on macOS (issue #96 item 2, Windows half). The work is in a PowerShell script the
        # package ships rather than in escaped one-liners here: it has to substitute the chosen
        # $INSTDIR into the task XML and write it as UTF-16, and neither is reviewable through
        # three layers of quoting. See packaging/windows/register-task.ps1.
        #
        # A failure is REPORTED, not fatal: a machine where schtasks refuses still gets a working
        # installation with a Start-menu shortcut, and the release gate asserts the task really
        # is registered, so a regression reddens CI rather than shipping quietly.
        #
        # THE QUOTES ARE ESCAPED TWICE, and this one is worse than the backslashes above because
        # it does not fail — it CORRUPTS. CPack writes this value into CPackConfig.cmake raw
        # (CPACK_VERBATIM_VARIABLES is off by default), so a bare double quote inside it closes
        # the string early; cpack then re-parses the remainder as a LIST and every quote comes
        # back as a semicolon. The result is a .nsi full of `DetailPrint ;Registering;the;`,
        # produced with no error at any stage. Measured by mirroring cpack_encode_variables()
        # exactly and re-parsing its output.
        #
        # StrCmp rather than LogicLib's ${If}: CPack's NSIS template includes MUI.nsh, Sections.nsh
        # and WinMessages.NSH and NOTHING else (checked in the template itself, not assumed), so
        # ${If} would fail to compile in makensis with an error naming a macro rather than a
        # missing include. StrCmp is core NSIS and needs nothing.
        set(CPACK_NSIS_EXTRA_INSTALL_COMMANDS "
    DetailPrint \\\"Registering the naudio logon task (disabled until you enable it)...\\\"
    nsExec::ExecToLog '\\\"$SYSDIR\\\\WindowsPowerShell\\\\v1.0\\\\powershell.exe\\\" -NoProfile -ExecutionPolicy Bypass -File \\\"$INSTDIR\\\\share\\\\naudio\\\\register-task.ps1\\\" -InstallDir \\\"$INSTDIR\\\"'
    Pop $0
    StrCmp $0 \\\"0\\\" +2 0
      DetailPrint \\\"naudio: the logon task could not be registered (code $0). Everything else is installed.\\\"
")
        # The same script with -Unregister, so the task NAME lives in exactly one file. The
        # release gate asserts the task is gone after an uninstall, which is also what would
        # catch this running after the payload had already been deleted.
        set(CPACK_NSIS_EXTRA_UNINSTALL_COMMANDS "
    nsExec::ExecToLog '\\\"$SYSDIR\\\\WindowsPowerShell\\\\v1.0\\\\powershell.exe\\\" -NoProfile -ExecutionPolicy Bypass -File \\\"$INSTDIR\\\\share\\\\naudio\\\\register-task.ps1\\\" -InstallDir \\\"$INSTDIR\\\" -Unregister'
    Pop $0
")
    endif()

    include(CPack)
endif()
