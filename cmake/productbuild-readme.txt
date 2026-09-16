Network Audio Service — what this installer places, and where

  /Applications/Network Audio Service.app
        The service, and the entry that opens its control page. The
        background service runs the program inside this app, so leave
        it where it is. Its switch is in System Settings > General >
        Login Items & Extensions > Allow in the Background, listed as
        Network Audio Service.

  /usr/local/bin        na_audio_daemon, na_wav_tap, and the demo pair
                        (na_audio_source, na_c_play_to_speakers)
  /usr/local/lib        the naudio shared library
  /usr/local/share/man  manual pages (man na_audio_daemon)

Getting started: open Network Audio Service from Applications. On its
Setup tab pick the radio's USB audio device and save; on Stream press
Start. The first time, macOS asks whether to allow Network Audio
Service to use the microphone — that is the radio's audio interface;
allow it. Nothing is captured until you press Start.

The service starts with your next login and serves the page at
http://127.0.0.1:8737/ (on this Mac only); it opens no device until
you ask it to.

Developers: this installer does not include headers or build files.
To compile against the naudio library, use the .tar.gz archive from
the same release page, or Homebrew.

To uninstall: switch the service off in Login Items, delete
/Applications/Network Audio Service.app and the files above, remove
/Library/LaunchAgents/org.kj5hst.naudio.daemon.plist, and forget the
package receipts:
  sudo pkgutil --forget org.kj5hst.naudio.runtime
  sudo pkgutil --forget org.kj5hst.naudio.tools
