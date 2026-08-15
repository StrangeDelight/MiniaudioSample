#!/usr/bin/env sh
#
# c - compile the program into ./play (Linux and macOS)
#
# Everything is compiled from src/ in one go: main.cpp (our code, which also
# pulls in miniaudio.h) plus RtMidi.cpp (the MIDI library). RtMidi has to be
# told which system MIDI API to use, so we look at the platform (uname) and
# pass the matching -D define, plus the system libraries to link against.

Pdir=$PWD
cd src

case "$(uname)" in
  Darwin)
    # macOS: RtMidi uses CoreMIDI, miniaudio uses CoreAudio - all part of macOS.
    MIDI_FLAGS="-D__MACOSX_CORE__"
    MIDI_LIBS="-framework CoreMIDI -framework CoreAudio -framework CoreFoundation"
    ;;
  *)
    # Linux: RtMidi uses ALSA, which needs the ALSA development files (see below).
    MIDI_FLAGS="-D__LINUX_ALSA__"
    MIDI_LIBS="-lasound"
    ;;
esac

echo "Compiling program for $(uname)"
if g++ -std=c++17 -O2 $MIDI_FLAGS $CXXFLAGS main.cpp RtMidi.cpp -o ../play \
       $LDFLAGS $MIDI_LIBS -lpthread -ldl -lm; then
  strip ../play
else
  echo
  echo "Compilation failed."
  if [ "$(uname)" != Darwin ] && [ ! -f /usr/include/alsa/asoundlib.h ]; then
    echo "The ALSA development files seem to be missing. Install them with:"
    echo "  Fedora / RHEL:    sudo dnf install alsa-lib-devel"
    echo "  Debian / Ubuntu:  sudo apt install libasound2-dev"
    echo "  Arch:             sudo pacman -S alsa-lib"
  fi
  if [ "$(uname)" = Darwin ] && ! command -v g++ >/dev/null 2>&1; then
    echo "No compiler found. Install the Xcode command line tools with:"
    echo "  xcode-select --install"
  fi
  cd "$Pdir"
  exit 1
fi
cd "$Pdir"

echo
echo "Use ./r                                              (mono C# sample, first MIDI keyboard)"
echo "Use ./r --port 2                                     (same, from MIDI port 2)"
echo "Use ./r samples/sample_stereo.raw --channels 2 --note 61   (stereo sample)"
echo "Use ./r other.raw --rate 24000 --note 60             (24000 Hz sample recorded at middle C)"
