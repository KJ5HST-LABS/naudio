naudio — network audio streaming toolkit

This installer places the naudio runtime library and command-line tools
under /usr/local:

  /usr/local/bin            na_audio_daemon, na_wav_tap, and the demo
                            pair (na_audio_source, na_c_play_to_speakers)
  /usr/local/lib            the naudio shared library
  /usr/local/share/man      manual pages (man na_audio_daemon)

Getting started: see docs/getting-started.md in the source repository,
or run `man na_audio_daemon` after installing.

Developers: this installer does not include headers or build files.
To compile against naudio, use the .tar.gz archive from the same
release page, or Homebrew.

To uninstall, remove the files above and forget the package receipt:
  sudo pkgutil --forget org.kj5hst.naudio.runtime
  sudo pkgutil --forget org.kj5hst.naudio.tools
