add_executable(naudio_tests
    test_types.cpp
    test_device_enumerator.cpp
    test_format_probe.cpp
    test_streams.cpp
    test_guide.cpp
    test_codec_primitives.cpp
    test_audio_packet.cpp
    test_protocol_types.cpp
    test_control_message.cpp
    test_jitter.cpp
    test_fec.cpp
    test_reorder.cpp
    test_control_reliability.cpp
    test_ring_buffer.cpp
    test_stream_rate.cpp
    test_format_conversion.cpp)

# Tests exercise the pure logic layer only — no PortAudio, no hardware.
# gtest_main links gtest transitively; linking both would duplicate libgtest.
# Threads::Threads: the AudioRingBuffer threaded test (blocking read + writer
# thread) needs pthread on Linux; harmless elsewhere.
find_package(Threads REQUIRED)
target_link_libraries(naudio_tests PRIVATE naudio_core GTest::gtest_main Threads::Threads)

# tools/ is on the include path for ONE header: na_stream_rate.h, na_hamlib_bridge's
# sample-rate negotiation policy (issue #84). It is deliberately not in include/naudio/ —
# that tree is installed and ships upstream, and this is one tool's policy, not library
# API. The header pulls in no hamlib and no naudio headers, so these arms build and run on
# the whole CI matrix while the bridge that uses it stays opt-in and POSIX-only.
target_include_directories(naudio_tests PRIVATE "${CMAKE_SOURCE_DIR}/tools")

include(GoogleTest)

# DISCOVERY_TIMEOUT on every gtest_discover_tests call in this file (issue #50).
# CMake's default is 5 s, and that is not enough here: on macOS the FIRST execution
# of a freshly linked binary pays signature / Gatekeeper validation. Measured on
# arm64 Sequoia, timing the exact --gtest_list_tests call discovery makes —
# naudio_tests 1.8 s, naudio_conformance_tests 19.5 s, naudio_net_tests 19.4 s cold,
# and 0.00 s once warm. A byte-identical `cp` of a warm binary costs 5.24 s on its
# first run, so the cost is keyed on file identity, not content: every relink is cold.
#
# The failure cannot self-heal, which is what makes it worth a comment. On timeout
# make DELETES the target, so the next build links a brand-new (cold) file, times out
# again, and deletes it again. A fresh clone plus a full build therefore fails on the
# project's primary development platform, with an error that points at the test
# executable rather than at the timeout. Only incremental builds that skip the relink
# escape it — which is why it went unnoticed for so long.
#
# 120 s is ~6x the worst measurement and costs nothing when discovery is fast: the
# value is a ceiling, not a wait.
set(NA_GTEST_DISCOVERY_TIMEOUT 120)

# A GTEST_SKIP()ped arm exits 0, so ctest counts it as PASSED and prints nothing —
# `--output-on-failure` shows the output of failures only. An arm that skips itself
# because its premise cannot be staged is therefore indistinguishable, in a CI log,
# from one that ran and proved something. That is the exact shape this project keeps
# being bitten by: an instrument whose silence is read as evidence.
#
# SKIP_REGULAR_EXPRESSION matches googletest's own "[  SKIPPED ]" banner and makes
# ctest report `***Skipped`, the way naudio_c_abi_stride already does via
# SKIP_RETURN_CODE 77 in the root CMakeLists. Skips still do not fail the run; they
# just stop being invisible. Applied to every gtest binary, not only the one that
# prompted it, because the reasoning is not specific to any arm.
# NOTE the trailing space, which is load-bearing. gtest_discover_tests writes each property
# value into the generated file as a CMake bracket argument, [[like this]] — so a value
# ENDING in "]" produces [[...\]]] and the bracket closes one character early, failing the
# whole generated file with "Argument not separated from preceding token by whitespace".
# googletest always prints a name after the banner ("[  SKIPPED ] Socket.Foo"), so matching
# the following space is both correct and enough to keep the value off a "]".
set(NA_GTEST_PROPERTIES PROPERTIES SKIP_REGULAR_EXPRESSION "\\[  SKIPPED \\] ")

gtest_discover_tests(naudio_tests DISCOVERY_TIMEOUT ${NA_GTEST_DISCOVERY_TIMEOUT}
                     ${NA_GTEST_PROPERTIES})

# ----------------------------------------------------------------------------
# Conformance harness — validates the C/C++ codec against the language-neutral
# golden vectors (conformance/vectors/vectors.ini, vendored into this repo).
# Links naudio_core (the pure codec), NOT the
# C ABI shared lib. The default vectors dir is baked in at configure time (the
# in-repo conformance/ dir at the project root); NA_CONFORMANCE_DIR overrides at
# runtime; absent → the test self-skips.
# ----------------------------------------------------------------------------
add_executable(naudio_conformance_tests conformance/ConformanceTest.cpp)
target_link_libraries(naudio_conformance_tests PRIVATE naudio_core GTest::gtest_main)
target_compile_definitions(naudio_conformance_tests
    PRIVATE NA_CONFORMANCE_DIR_DEFAULT="${CMAKE_SOURCE_DIR}/conformance")
gtest_discover_tests(naudio_conformance_tests DISCOVERY_TIMEOUT ${NA_GTEST_DISCOVERY_TIMEOUT}
                     ${NA_GTEST_PROPERTIES})

# ----------------------------------------------------------------------------
# Net transport tests — exercise the naudio_net layer (sockets, the
# framing FSM, the TCP transports) over OS loopback. Hardware-free. Separate
# executable because it links naudio_net (sockets + threads), not just the pure
# naudio_core the codec/reliability tests use.
# ----------------------------------------------------------------------------
add_executable(naudio_net_tests
    net/test_socket.cpp
    net/test_transport_types.cpp
    net/test_protocol_handler.cpp
    net/test_tcp_transport.cpp
    net/test_udp_connection.cpp
    net/test_udp_transport.cpp
    net/test_discovery.cpp
    net/test_dual_transport.cpp
    net/test_broadcaster.cpp
    net/test_mixer.cpp
    net/test_server.cpp
    net/test_client_e2e.cpp
    net/test_format_negotiation.cpp
    # The lossy-relay fixture (shared with the C stats test's own target).
    client_stats_proxy.cpp)
target_link_libraries(naudio_net_tests PRIVATE naudio_net GTest::gtest_main Threads::Threads)
gtest_discover_tests(naudio_net_tests DISCOVERY_TIMEOUT ${NA_GTEST_DISCOVERY_TIMEOUT}
                     ${NA_GTEST_PROPERTIES})
