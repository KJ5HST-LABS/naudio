# Third-Party Notices

naudio itself is licensed under **LGPL-2.1-or-later** (see [LICENSE](LICENSE)). It uses the
third-party components below. None of their source is vendored into this repository; each is
resolved at build time from the system or via CMake `FetchContent`.

## PortAudio

- **Role:** cross-platform audio device I/O backend (`naudio_pa`).
- **License:** the PortAudio license (MIT-style). See <https://www.portaudio.com/license.html>.
- **How it is used:**
  - **Supported path — dynamic link.** naudio links a system / package-manager PortAudio
    (`pkg-config portaudio-2.0`). PortAudio stays a separate shared library under its own license
    and is not redistributed by naudio.
  - **FetchContent path — static bundle.** When no system PortAudio is found, the build fetches and
    statically links PortAudio v19.7.0. A binary produced this way **embeds** PortAudio; if you
    redistribute such a binary you must comply with and reproduce the PortAudio license. This path
    is intended for local / CI builds without a system PortAudio — for redistribution, prefer the
    dynamic (system-PortAudio) path.

## GoogleTest

- **Role:** unit-test framework, used by the test targets only.
- **License:** BSD-3-Clause. See <https://github.com/google/googletest/blob/main/LICENSE>.
- **How it is used:** resolved via `find_package(GTest)` or `FetchContent` for the test build only.
  It is **not** installed or shipped (`INSTALL_GTEST OFF`) and is absent from the installed package
  (`libnaudio`, the headers, and `naudio.pc`).
