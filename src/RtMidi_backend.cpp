/*

  RtMidi_backend.cpp - compiles the RtMidi library for the platform we are on

  RtMidi has to be told which system MIDI API to use. Instead of passing that
  as a compiler flag from the build script, we choose it here from the
  compiler's built-in platform defines and then include RtMidi.cpp itself.
  That way one plain "g++ main.cpp RtMidi_backend.cpp" works on both Linux
  and macOS; only the libraries on the link line differ (see ./c).

*/

#if defined(__APPLE__)
  #define __MACOSX_CORE__     // CoreMIDI, part of macOS: link CoreMIDI/CoreAudio/CoreFoundation
#elif defined(__linux__)
  #define __LINUX_ALSA__      // ALSA sequencer: needs alsa-lib-devel / libasound2-dev, link -lasound
#else
  #error "Unsupported platform: add the RtMidi API define for it here (see RtMidi.h)"
#endif

#include "RtMidi.cpp"
