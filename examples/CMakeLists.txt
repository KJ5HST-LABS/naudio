# ----------------------------------------------------------------------------
# examples/ — reference consumers of the PUBLIC na_* C ABI.
#
# Unlike tools/ (the hardware-smoke daemon, which links the
# INTERNAL static C++ targets naudio_net/naudio_pa directly), the examples here
# link ONLY the public hidden-visibility shared `naudio` and #include only
# `naudio.h` — they are the proof that the C ABI is self-sufficient from pure C.
# Gated by NAUDIO_BUILD_APPS, like the tools/ apps. The example clients use
# POSIX-only timing / atomics APIs (nanosleep, clock_gettime, <stdatomic.h>), so —
# like tools/ — they build only on non-Windows platforms.
# ----------------------------------------------------------------------------
if(NAUDIO_BUILD_APPS AND NOT WIN32)
    # na_audio_source — the demo audio source for the examples suite. A pure-C
    # streaming server over the na_server_* ABI: it captures a local PortAudio
    # input and broadcasts it (so a client plays it on local speakers), or emits
    # a hardware-free deterministic test tone. Links the public shared `naudio`
    # (the C ABI) ONLY — never the internal C++ classes. Linked as C++ because
    # libnaudio carries the C++ runtime (mirrors na_c_play_to_speakers).
    add_executable(na_audio_source server/audio_source.c)
    target_link_libraries(na_audio_source PRIVATE naudio)
    set_target_properties(na_audio_source PROPERTIES LINKER_LANGUAGE CXX)

    # na_c_play_to_speakers — the C client of the examples suite, and the reference
    # template the other-language clients copy. A pure-C program over the na_client_*
    # ABI: by default it picks the first output device and plays the received stream
    # on the local speakers. Links the public shared `naudio` (the C ABI) ONLY — never
    # the internal C++ classes. Linked as C++ because libnaudio carries the C++ runtime
    # (mirrors na_audio_source).
    add_executable(na_c_play_to_speakers c/play_to_speakers.c)
    target_link_libraries(na_c_play_to_speakers PRIVATE naudio)
    set_target_properties(na_c_play_to_speakers PROPERTIES LINKER_LANGUAGE CXX)

    # na_cpp_play_to_speakers — the C++ client of the examples suite. The SAME program as
    # na_c_play_to_speakers, compiled as C++: it #includes only naudio.h and links only the
    # public shared `naudio`, never the internal C++ class AudioStreamClient.hpp — so it is the
    # proof that the flat C ABI is self-sufficient from C++ too (not just from plain C).
    add_executable(na_cpp_play_to_speakers cpp/play_to_speakers.cpp)
    target_link_libraries(na_cpp_play_to_speakers PRIVATE naudio)
elseif(NAUDIO_BUILD_APPS AND WIN32)
    message(STATUS "naudio: examples/ use POSIX-only APIs (nanosleep/clock_gettime/<stdatomic.h>); skipped on Windows.")
endif()
