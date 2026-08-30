# ----------------------------------------------------------------------------
# examples/ — reference consumers of the PUBLIC na_* C ABI.
#
# Unlike tools/ (the hardware-smoke daemon, which links the
# INTERNAL static C++ targets naudio_net/naudio_pa directly), the examples here
# link ONLY the public hidden-visibility shared `naudio` and #include only
# `naudio.h` — they are the proof that the C ABI is self-sufficient from pure C.
# Gated by NAUDIO_BUILD_APPS, like the tools/ apps. The SHIPPED demo pair
# (na_audio_source + na_c_play_to_speakers) builds on every platform — their two
# POSIX time calls are shimmed like tools/na_wav_tap.c, and their C11 atomics
# come from MSVC's /experimental:c11atomics on Windows. The C++ twin below stays
# POSIX-only: it proves an ABI property the C pair already proves, and ships in
# no package.
# ----------------------------------------------------------------------------
if(NAUDIO_BUILD_APPS)
    # na_audio_source — the demo audio source for the examples suite. A pure-C
    # streaming server over the na_server_* ABI: it captures a local PortAudio
    # input and broadcasts it (so a client plays it on local speakers), or emits
    # a hardware-free deterministic test tone. Links the public shared `naudio`
    # (the C ABI) ONLY — never the internal C++ classes. Linked as C++ because
    # libnaudio carries the C++ runtime (mirrors na_c_play_to_speakers).
    add_executable(na_audio_source server/audio_source.c)
    target_link_libraries(na_audio_source PRIVATE naudio)
    set_target_properties(na_audio_source PROPERTIES LINKER_LANGUAGE CXX
                          C_STANDARD 11 C_STANDARD_REQUIRED ON)
    if(MSVC)
        # <stdatomic.h> in C mode: MSVC ships it behind this flag (VS2022 17.5+).
        target_compile_options(na_audio_source PRIVATE /experimental:c11atomics)
    endif()
    # Ships in the binary packages (with na_c_play_to_speakers below): together
    # they are the out-of-the-box demo — start a test-tone server on one
    # machine, hear it on another, no code and no audio input required.
    install(TARGETS na_audio_source
            RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT tools)
    if(NOT WIN32)  # the Windows ZIP has no man tree (same rule as na_wav_tap's page)
        install(FILES "${naudio_SOURCE_DIR}/docs/man/na_audio_source.1"
                DESTINATION "${CMAKE_INSTALL_MANDIR}/man1" COMPONENT tools)
    endif()

    # na_c_play_to_speakers — the C client of the examples suite, and the reference
    # template the other-language clients copy. A pure-C program over the na_client_*
    # ABI: by default it picks the first output device and plays the received stream
    # on the local speakers. Links the public shared `naudio` (the C ABI) ONLY — never
    # the internal C++ classes. Linked as C++ because libnaudio carries the C++ runtime
    # (mirrors na_audio_source).
    add_executable(na_c_play_to_speakers c/play_to_speakers.c)
    target_link_libraries(na_c_play_to_speakers PRIVATE naudio)
    set_target_properties(na_c_play_to_speakers PROPERTIES LINKER_LANGUAGE CXX
                          C_STANDARD 11 C_STANDARD_REQUIRED ON)
    if(MSVC)
        target_compile_options(na_c_play_to_speakers PRIVATE /experimental:c11atomics)
    endif()
    # The listening half of the shipped demo pair (see na_audio_source above).
    install(TARGETS na_c_play_to_speakers
            RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT tools)
    if(NOT WIN32)
        install(FILES "${naudio_SOURCE_DIR}/docs/man/na_c_play_to_speakers.1"
                DESTINATION "${CMAKE_INSTALL_MANDIR}/man1" COMPONENT tools)
    endif()

    if(NOT WIN32)
        # na_cpp_play_to_speakers — the C++ client of the examples suite. The SAME program as
        # na_c_play_to_speakers, compiled as C++: it #includes only naudio.h and links only the
        # public shared `naudio`, never the internal C++ class AudioStreamClient.hpp — so it is the
        # proof that the flat C ABI is self-sufficient from C++ too (not just from plain C).
        add_executable(na_cpp_play_to_speakers cpp/play_to_speakers.cpp)
        target_link_libraries(na_cpp_play_to_speakers PRIVATE naudio)
    else()
        message(STATUS "naudio: na_cpp_play_to_speakers uses POSIX-only APIs; skipped on Windows "
                       "(the shipped demo pair na_audio_source/na_c_play_to_speakers builds).")
    endif()
endif()
