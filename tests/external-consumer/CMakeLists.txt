# Throwaway external consumer of the INSTALLED naudio package (installed-library check).
# Configured in isolation against an install prefix (NOT a subdirectory of the
# naudio build): CMAKE_PREFIX_PATH=<prefix> cmake -S . -B build. Resolves naudio
# purely through the installed naudioConfig.cmake — no source-tree paths.
cmake_minimum_required(VERSION 3.20)
project(naudio_external_consumer LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(naudio CONFIG REQUIRED)

# C consumer: client + server C ABI through the shared library naudio::naudio.
add_executable(consume_c consume_c.c)
target_link_libraries(consume_c PRIVATE naudio::naudio)

# C++ consumer: the C++ API through the installed static archives.
add_executable(consume_cxx consume_cxx.cpp)
target_link_libraries(consume_cxx PRIVATE naudio::naudio_net naudio::naudio_core)
