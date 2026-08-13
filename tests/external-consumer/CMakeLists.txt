# Throwaway external consumer of the INSTALLED naudio package (installed-library check).
# Configured in isolation against an install prefix (NOT a subdirectory of the
# naudio build): CMAKE_PREFIX_PATH=<prefix> cmake -S . -B build. Resolves naudio
# purely through the installed naudioConfig.cmake — no source-tree paths.
cmake_minimum_required(VERSION 3.20)
project(naudio_external_consumer LANGUAGES C CXX)

# _REQUIRED ON on both: without it CMake silently DECAYS to whatever the toolchain supports, which
# is the same "the level is whatever you happened to get" defect as issue #42, one layer out. The
# consumer only needs C99 (naudio.h is C99-clean), but this gate should fail loudly rather than
# quietly compile at a level nobody chose.
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(naudio CONFIG REQUIRED)

# C consumer: client + server C ABI through the shared library naudio::naudio.
add_executable(consume_c consume_c.c)
target_link_libraries(consume_c PRIVATE naudio::naudio)

# C++ consumer: the C++ API through the installed static archives.
add_executable(consume_cxx consume_cxx.cpp)
target_link_libraries(consume_cxx PRIVATE naudio::naudio_net naudio::naudio_core)

# Device-backend consumer: naudio::naudio_pa, the ONLY installed target whose
# PortAudio dependency the consumer must resolve itself (issue #61). Conditional
# because that is the actual contract — naudioConfig.cmake sets
# naudio_PORTAUDIO_FOUND FALSE on a host with no PortAudio, where the package still
# loads and every other target here still links.
#
# A silently-skipped arm is how this project has been fooled before, so the status
# below is printed either way and run.sh asserts on the line, never on exit status.
if(naudio_PORTAUDIO_FOUND)
    add_executable(consume_pa consume_pa.cpp)
    target_link_libraries(consume_pa PRIVATE naudio::naudio_pa)
    message(STATUS "external-consumer: naudio_pa arm ENABLED (naudio_PORTAUDIO_FOUND)")
else()
    message(STATUS "external-consumer: naudio_pa arm SKIPPED — no consumer-side PortAudio")
endif()
