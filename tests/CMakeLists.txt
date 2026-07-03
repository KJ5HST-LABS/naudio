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
    test_ring_buffer.cpp)

# Tests exercise the pure logic layer only — no PortAudio, no hardware.
# gtest_main links gtest transitively; linking both would duplicate libgtest.
# Threads::Threads: the AudioRingBuffer threaded test (blocking read + writer
# thread) needs pthread on Linux; harmless elsewhere.
find_package(Threads REQUIRED)
target_link_libraries(naudio_tests PRIVATE naudio_core GTest::gtest_main Threads::Threads)

include(GoogleTest)
gtest_discover_tests(naudio_tests)

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
gtest_discover_tests(naudio_conformance_tests)

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
    net/test_dual_transport.cpp
    net/test_broadcaster.cpp
    net/test_mixer.cpp
    net/test_server.cpp
    net/test_client_e2e.cpp)
target_link_libraries(naudio_net_tests PRIVATE naudio_net GTest::gtest_main Threads::Threads)
gtest_discover_tests(naudio_net_tests)
