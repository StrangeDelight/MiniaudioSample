/*

  play - plays a raw PCM sample from a MIDI keyboard

    play <file.raw> [--rate N] [--channels N] [--note N] [--port N]

      file.raw     raw signed 16-bit little-endian PCM samples
      --rate N     native sample rate of the material in Hz  (default 48000)
      --channels N 1 = mono, 2 = stereo (L,R,L,R...)         (default 1)
      --note N     MIDI note that plays the sample at native pitch, i.e. the
                   note the sample was recorded at            (default 60 = middle C)
      --port N     MIDI input port number                     (default: first keyboard)

    Every key on the keyboard plays the sample. The key given by --note plays
    it at its native pitch; each key above or below shifts it by one
    semitone. Several keys can sound at the same time. Releasing a key
    fades the sample out. Press Enter to quit.

    examples
      play sample.raw                          48000 Hz mono sample, root note C
      play sample.raw --note 61                the sample is a C#
      play sample.raw --rate 24000             the material was recorded at 24000 Hz
      play stereo.raw --channels 2             stereo sample
      play sample.raw --port 2                 use MIDI input port 2 (see list at start)

  Building: miniaudio (audio out) and RtMidi (MIDI in) are both included in
  this folder, so nothing extra is needed for them. Run ./c to compile and
  ./r to play. Works on Linux and macOS:
    Linux   RtMidi talks to the keyboard through ALSA, so the ALSA development
            files must be installed (Fedora: alsa-lib-devel,
            Debian/Ubuntu: libasound2-dev). Audio goes through ALSA/PipeWire.
    macOS   RtMidi uses CoreMIDI and miniaudio uses CoreAudio, both part of
            the system; only the Xcode command line tools are needed
            (xcode-select --install).
  ./c tells RtMidi which system MIDI API to use (-D__LINUX_ALSA__ or
  -D__MACOSX_CORE__); the few platform differences in this file are marked
  with #if defined(__APPLE__).

  Note: if the keyboard is unplugged and plugged back in while the program
  runs, the connection to it is lost. Just quit (Enter) and start it again.

  How it works, thread by thread:
    main thread    parses arguments, loads the sample, opens audio and MIDI,
                   then just waits for Enter.
    MIDI thread    (owned by RtMidi) calls midi_callback for every message
                   from the keyboard; note on/off events go into a queue.
    audio thread   (owned by miniaudio) calls data_callback whenever the
                   sound card wants more samples; it takes events from the
                   queue, starts/stops voices and mixes them into the output.

*/


#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
#include "RtMidi.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>
#include <vector>


constexpr long kDeviceRate      = 48000;  // Hz, what we ask the sound card for
constexpr long kDefaultRate     = 48000;  // Hz, native rate of the sample material
constexpr long kDefaultChannels = 1;
constexpr long kDefaultRootNote = 60;     // middle C plays the sample at native pitch
constexpr int  kMaxVoices       = 16;     // how many keys can sound at the same time
constexpr int  kReleaseMs       = 30;     // fade-out after a key is released (avoids clicks)
constexpr int  kFixedVelocity   = 100;    // every key plays at this velocity (1..127);
                                          // set to 0 to use the real velocity from the keyboard
constexpr int  kQueueSize       = 256;    // MIDI events waiting for the audio thread (power of two!)


// ---------------------------------------------------------------------------
// The sample
// ---------------------------------------------------------------------------

static std::vector<int16_t> g_samples;    // interleaved 16-bit samples from the file
static size_t g_frames = 0;               // number of frames (one frame = one sample per channel)
static long g_channels = kDefaultChannels;
static long g_sampleRate = kDefaultRate;  // the rate the material was recorded at
static long g_rootNote = kDefaultRootNote; // the MIDI note the material was recorded at


// ---------------------------------------------------------------------------
// MIDI events: from the MIDI thread to the audio thread
// ---------------------------------------------------------------------------

// A MIDI channel message: status byte + two data bytes.
//   note on   status 0x9n, data1 = note, data2 = velocity (0 means note off)
//   note off  status 0x8n, data1 = note, data2 = release velocity (unused)
struct MidiEvent {
    uint8_t status = 0;
    uint8_t data1 = 0;
    uint8_t data2 = 0;
};

// Events travel from the MIDI thread to the audio thread through this small
// ring buffer. Exactly one thread writes (MIDI) and one thread reads (audio),
// so two atomic counters are enough: no mutex, so the audio thread never
// has to wait. The counters only ever grow; the slot is counter % kQueueSize.
static MidiEvent g_queue[kQueueSize];
static std::atomic<unsigned> g_queueWritten{0};   // events pushed so far (MIDI thread)
static std::atomic<unsigned> g_queueRead{0};      // events taken so far (audio thread)

// MIDI thread: append an event. If the queue is full the event is dropped,
// which only happens if the audio thread has stalled for a long time.
static void push_event(MidiEvent event) {
    unsigned written = g_queueWritten.load();
    if (written - g_queueRead.load() >= kQueueSize) return;
    g_queue[written % kQueueSize] = event;
    g_queueWritten.store(written + 1);
}

// Audio thread: take the next event, or return false if there is none.
static bool pop_event(MidiEvent* event) {
    unsigned read = g_queueRead.load();
    if (read == g_queueWritten.load()) return false;
    *event = g_queue[read % kQueueSize];
    g_queueRead.store(read + 1);
    return true;
}

// A short human-readable name for a MIDI status byte, for the console dump.
static const char* describe_status(uint8_t status) {
    switch (status & 0xF0) {
        case 0x80: return "note off";
        case 0x90: return "note on";
        case 0xA0: return "poly aftertouch";
        case 0xB0: return "control change";
        case 0xC0: return "program change";
        case 0xD0: return "channel pressure";
        case 0xE0: return "pitch bend";
        default:   return status == 0xF0 ? "sysex" : "system";
    }
}

// Called by RtMidi's thread for every MIDI message from the keyboard. Every
// message is dumped to the console (raw bytes + what it is) so you can see
// exactly what the keyboard sends; only note on/off go on to the audio thread.
static void midi_callback(double /*timestamp*/, std::vector<unsigned char>* message,
                          void* /*userData*/) {
    if (message->empty()) return;
    const uint8_t status = (*message)[0];

    std::printf("  midi:");
    for (unsigned char byte : *message) std::printf(" %02X", byte);
    std::printf("   %s", describe_status(status));
    if (message->size() >= 3 && ((status & 0xF0) == 0x90 || (status & 0xF0) == 0x80)) {
        std::printf(" %3u velocity %3u", (*message)[1], (*message)[2]);
    }
    std::printf("\n");
    std::fflush(stdout);

    if (message->size() < 3) return;                // not a channel message with 2 data bytes
    MidiEvent event{status, (*message)[1], (*message)[2]};
    uint8_t type = status & 0xF0;                   // top nibble = message type
    if (type != 0x90 && type != 0x80) return;        // only note on / note off
    push_event(event);
}


// ---------------------------------------------------------------------------
// Voices: one for every key that is currently sounding
// ---------------------------------------------------------------------------

struct Voice {
    bool   active = false;
    int    note = 0;
    double position = 0.0;    // where we are in the sample, in frames (fractional)
    double step = 1.0;        // frames to advance per output frame (1.0 = native pitch)
    float  volume = 1.0f;     // from the key velocity, fixed for the life of the voice
    float  gain = 1.0f;       // 1 while the key is held, ramps down to 0 after release
    float  gainStep = 0.0f;   // how much gain drops per frame while releasing (0 = held)
};

static Voice g_voices[kMaxVoices];

// How fast to move through the sample for a given key. At the root note the
// sample plays at its native rate (a 24000 Hz sample on a 48000 Hz device
// means half a frame per output frame). Each semitone up multiplies the
// speed by the twelfth root of two.
static double note_to_step(int note) {
    double native = (double)g_sampleRate / (double)kDeviceRate;
    return native * std::pow(2.0, (note - g_rootNote) / 12.0);
}

// Key velocity (1..127) to volume (0..1). Our ears hear loudness roughly
// logarithmically, so a straight velocity/127 makes soft keys almost silent;
// the square root keeps them audible while hard keys stay louder.
static float velocity_to_volume(int velocity) {
    return std::sqrt(velocity / 127.0f);
}

// Start the sample on a free voice. If all voices are busy, take over the
// one that is furthest through the sample.
static void note_on(int note, int velocity) {
    Voice* voice = nullptr;
    for (Voice& v : g_voices) {
        if (!v.active) { voice = &v; break; }
        if (voice == nullptr || v.position > voice->position) voice = &v;
    }
    voice->active = true;
    voice->note = note;
    voice->position = 0.0;
    voice->step = note_to_step(note);
    if (kFixedVelocity > 0) velocity = kFixedVelocity;   // ignore how hard the key was hit
    voice->volume = velocity_to_volume(velocity);
    voice->gain = 1.0f;
    voice->gainStep = 0.0f;
}

// Start fading out every voice that plays this note.
static void note_off(int note) {
    const float releaseFrames = (float)kReleaseMs * (float)kDeviceRate / 1000.0f;
    for (Voice& v : g_voices) {
        if (v.active && v.note == note && v.gainStep == 0.0f) {
            v.gainStep = 1.0f / releaseFrames;
        }
    }
}

static void handle_event(const MidiEvent& event) {
    uint8_t type = event.status & 0xF0;
    if (type == 0x90 && event.data2 > 0) note_on(event.data1, event.data2);
    else                                 note_off(event.data1);
}

// Fill 'out' with frameCount frames of audio: apply pending MIDI events, then
// mix all active voices. Runs on the audio thread, so no printing, no
// blocking, no memory allocation in here.
static void render_audio(int16_t* out, size_t frameCount) {
    MidiEvent event;
    while (pop_event(&event)) handle_event(event);

    for (size_t f = 0; f < frameCount; ++f) {
        float mix[2] = {0.0f, 0.0f};              // one accumulator per channel

        for (Voice& v : g_voices) {
            if (!v.active) continue;

            // Read the sample at a fractional position: blend the frame we
            // are on with the next one (linear interpolation). This is what
            // makes playback at other speeds sound smooth instead of gritty.
            size_t index = (size_t)v.position;
            float fraction = (float)(v.position - (double)index);
            const int16_t* here = &g_samples[index * g_channels];
            const int16_t* next = here + g_channels;
            float amplitude = v.volume * v.gain;
            for (long c = 0; c < g_channels; ++c) {
                float value = here[c] + (next[c] - here[c]) * fraction;
                mix[c] += value * amplitude;
            }

            v.position += v.step;
            if (v.gainStep > 0.0f) v.gain -= v.gainStep;
            if (v.gain <= 0.0f || v.position >= (double)(g_frames - 1)) {
                v.active = false;                 // released or ran off the end
            }
        }

        // Clamp to the 16-bit range so loud chords distort instead of wrapping.
        for (long c = 0; c < g_channels; ++c) {
            float value = mix[c];
            if (value > 32767.0f) value = 32767.0f;
            if (value < -32768.0f) value = -32768.0f;
            out[f * g_channels + c] = (int16_t)value;
        }
    }
}


// ---------------------------------------------------------------------------
// Audio device (miniaudio)
// ---------------------------------------------------------------------------

static ma_device g_device;

// Called by miniaudio's audio thread whenever the device wants more frames.
static void data_callback(ma_device* /*device*/, void* output, const void* /*input*/,
                          ma_uint32 frameCount) {
    render_audio(static_cast<int16_t*>(output), frameCount);
}

// Open the default playback device: s16 samples at the requested rate and
// channel count, fed by data_callback. Exits nonzero on failure.
static void init_audio() {
    ma_device_config config = ma_device_config_init(ma_device_type_playback);
    config.playback.format = ma_format_s16;
    config.playback.channels = (ma_uint32)g_channels;
    config.sampleRate = (ma_uint32)kDeviceRate;
    config.dataCallback = data_callback;

    if (ma_device_init(nullptr, &config, &g_device) != MA_SUCCESS) {
        std::fprintf(stderr, "error: failed to open playback device\n");
        std::exit(1);
    }
    if (ma_device_start(&g_device) != MA_SUCCESS) {
        std::fprintf(stderr, "error: failed to start playback device\n");
        ma_device_uninit(&g_device);
        std::exit(1);
    }
}


// ---------------------------------------------------------------------------
// MIDI input (RtMidi)
// ---------------------------------------------------------------------------

// Created in init_midi (the constructor can fail, so not a plain global).
static std::unique_ptr<RtMidiIn> g_midi;

// Every system offers a virtual "loopback" MIDI port that is not a keyboard.
// When no --port is given we skip it and take the first other port.
#if defined(__APPLE__)
static const char* kVirtualPortName = "IAC Driver";    // macOS inter-application bus
#else
static const char* kVirtualPortName = "Midi Through";  // ALSA loopback on Linux
#endif

// List the MIDI input ports, open the requested one (or the first one that
// looks like a real keyboard) and route its messages to midi_callback.
static void init_midi(long midiport) {
    try {
        g_midi.reset(new RtMidiIn());
    } catch (RtMidiError& error) {
        std::fprintf(stderr, "error: cannot start MIDI: %s\n", error.getMessage().c_str());
        std::exit(1);
    }

    unsigned count = g_midi->getPortCount();
    if (count == 0) {
        std::fprintf(stderr, "error: no MIDI input ports found - is the keyboard plugged in?\n");
        std::exit(1);
    }
    std::printf("MIDI input ports:\n");
    for (unsigned i = 0; i < count; ++i) {
        std::printf("  %u: %s\n", i, g_midi->getPortName(i).c_str());
    }

    unsigned port = 0;
    if (midiport >= 0) {
        if ((unsigned)midiport >= count) {
            std::fprintf(stderr, "error: MIDI port %ld does not exist\n", midiport);
            std::exit(1);
        }
        port = (unsigned)midiport;
    } else {
        // Prefer anything that is not the system's virtual loopback port,
        // since that is most likely the actual keyboard.
        for (unsigned i = 0; i < count; ++i) {
            if (g_midi->getPortName(i).find(kVirtualPortName) == std::string::npos) {
                port = i;
                break;
            }
        }
    }

    // Install the callback BEFORE opening the port: anything that arrives
    // before a callback is set goes into RtMidi's internal queue and would
    // never reach us.
    g_midi->setCallback(midi_callback);
    g_midi->ignoreTypes(false, false, false); // deliver everything, even sysex/timing/active sensing,
                                              // so the console dump shows all the keyboard sends
    try {
        g_midi->openPort(port);
    } catch (RtMidiError& error) {
        std::fprintf(stderr, "error: cannot open MIDI port %u: %s\n", port,
                     error.getMessage().c_str());
        std::exit(1);
    }
    std::printf("Listening on port %u: %s\n", port, g_midi->getPortName(port).c_str());
}


// ---------------------------------------------------------------------------
// Command line and sample loading
// ---------------------------------------------------------------------------

// Parse a whole decimal number that is at least 'minimum'.
static bool parse_number(const char* text, long minimum, long* out) {
    char* end = nullptr;
    long value = std::strtol(text, &end, 10);
    if (end == text || *end != '\0' || value < minimum) return false;
    *out = value;
    return true;
}

static void print_usage_and_exit(const char* program) {
    std::fprintf(stderr,
        "Usage: %s <file.raw> [--rate N] [--channels N] [--note N] [--port N]\n"
        "  file.raw      raw signed 16-bit little-endian PCM bytes\n"
        "  --rate N      native sample rate of the material in Hz (default 48000)\n"
        "  --channels N  1 = mono (default), 2 = stereo interleaved L,R,L,R...\n"
        "  --note N      MIDI note that plays the sample at native pitch (default 60)\n"
        "  --port N      MIDI input port number (default: first keyboard found)\n",
        program);
    std::exit(1);
}

// Parse argv: one file name plus any number of "--option value" pairs, in any
// order. Fills the globals and midiport (-1 = "auto"); prints usage or an
// error and exits nonzero on bad input.
static void read_arguments(int argc, char** argv, long* midiport, const char** datafile) {
    g_sampleRate = kDefaultRate;
    g_channels = kDefaultChannels;
    g_rootNote = kDefaultRootNote;
    *midiport = -1;
    *datafile = nullptr;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg.rfind("--", 0) != 0) {                // does not start with "--": the file
            if (*datafile != nullptr) {
                std::fprintf(stderr, "error: unexpected argument '%s'\n", argv[i]);
                print_usage_and_exit(argv[0]);
            }
            *datafile = argv[i];
            continue;
        }
        if (i + 1 >= argc) {
            std::fprintf(stderr, "error: %s needs a value\n", argv[i]);
            print_usage_and_exit(argv[0]);
        }
        const char* value = argv[++i];                 // the word after the option
        bool ok = false;
        if      (arg == "--rate")     ok = parse_number(value, 1, &g_sampleRate);
        else if (arg == "--channels") ok = parse_number(value, 1, &g_channels) && g_channels <= 2;
        else if (arg == "--note")     ok = parse_number(value, 0, &g_rootNote) && g_rootNote <= 127;
        else if (arg == "--port")     ok = parse_number(value, 0, midiport);
        else {
            std::fprintf(stderr, "error: unknown option '%s'\n", arg.c_str());
            print_usage_and_exit(argv[0]);
        }
        if (!ok) {
            std::fprintf(stderr, "error: '%s' is not a valid value for %s\n", value, arg.c_str());
            std::exit(1);
        }
    }
    if (*datafile == nullptr) print_usage_and_exit(argv[0]);
}

// Read the whole file into the sample buffer; exit nonzero if it is
// unreadable or too short. Leftover bytes that don't fill a frame are dropped.
static void load_samples(const char* datafile) {
    std::ifstream file(datafile, std::ios::binary | std::ios::ate);
    if (!file) {
        std::fprintf(stderr, "error: cannot open '%s'\n", datafile);
        std::exit(1);
    }
    const size_t bytes = (size_t)file.tellg();
    const size_t bytesPerFrame = sizeof(int16_t) * (size_t)g_channels;
    g_frames = bytes / bytesPerFrame;
    if (g_frames < 2) {
        std::fprintf(stderr, "error: '%s' is empty or too short\n", datafile);
        std::exit(1);
    }
    g_samples.resize(g_frames * (size_t)g_channels);
    file.seekg(0);
    file.read(reinterpret_cast<char*>(g_samples.data()), (std::streamsize)(g_frames * bytesPerFrame));

    std::printf("Loaded %zu bytes: %zu frames at %ld Hz, %ld channel(s) (~%.2f s), root note %ld\n",
                bytes, g_frames, g_sampleRate, g_channels,
                (double)g_frames / (double)g_sampleRate, g_rootNote);
}

// Block until the user presses Enter, then shut everything down in order:
// MIDI first (no new events), then the audio device.
static void wait_for_enter() {
    std::printf("Play some keys! Press Enter to quit.\n");
    std::getchar();
    g_midi->closePort();
    g_midi.reset();
    ma_device_uninit(&g_device);
}

// main program
// parse commandline arguments
// load samples
// init audio (starts the audio thread)
// init midi  (starts the MIDI thread)
// wait for Enter
// done
int main(int argc, char** argv) {
    long midiport = -1;
    const char* datafile = nullptr;

    read_arguments(argc, argv, &midiport, &datafile);
    load_samples(datafile);
    init_audio();
    init_midi(midiport);
    wait_for_enter();
    return 0;
}
