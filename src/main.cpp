
/*

  play - plays raw PCM sample bytes through the speakers

    play <file.raw> [rate] [channels]

      file.raw   raw signed 16-bit little-endian PCM samples
      rate       playback sample rate in Hz         (default 48000)
      channels   1 = mono, 2 = stereo (L,R,L,R...)  (default 1)

    examples
      play sample.raw            native pitch (48000 Hz mono)
      play sample.raw 24000      an octave lower
      play stereo.raw 48000 2    stereo

*/


#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <thread>
#include <vector>


constexpr long kDefaultRate     = 48000;  // Hz, matches the sample material
constexpr long kDefaultChannels = 1;
constexpr int  kPollIntervalMs  = 10;     // how often we check the done flag
constexpr int  kDrainMs         = 100;    // let the backend finish the last buffer


struct PlaybackState {
    const uint8_t* data = nullptr;
    size_t size = 0;
    size_t cursor = 0;              // only touched by the audio thread
    std::atomic<bool> done{false};
};

// File-scope state so main reads as a plain sequence of calls.
static std::vector<uint8_t> g_samples;
static PlaybackState g_state;
static ma_device g_device;

// Called by miniaudio's audio thread whenever the device wants more frames.
// No need to change this
static void data_callback(ma_device* device, void* output, const void*,
                          ma_uint32 frameCount) {
    auto* state = static_cast<PlaybackState*>(device->pUserData);
    const size_t bytesPerFrame = sizeof(int16_t) * device->playback.channels;
    const size_t wanted = (size_t)frameCount * bytesPerFrame;
    const size_t remaining = state->size - state->cursor;
    const size_t copied = remaining < wanted ? remaining : wanted;

    std::memcpy(output, state->data + state->cursor, copied);
    state->cursor += copied;

    if (copied < wanted) {  // ran out of samples: pad silence, signal main
        std::memset(static_cast<uint8_t*>(output) + copied, 0, wanted - copied);
        state->done.store(true);
    }
}

static bool parse_positive(const char* text, long* out) {
    char* end = nullptr;
    long value = std::strtol(text, &end, 10);
    if (end == text || *end != '\0' || value <= 0) return false;
    *out = value;
    return true;
}

// Parse argv into rate/channels/datafile, applying defaults; print usage or
// an error and exit nonzero on bad input.
static void read_arguments(int argc, char** argv, long* rate, long* channels,
                           const char** datafile) {
    *rate = kDefaultRate;
    *channels = kDefaultChannels;

    if (argc < 2 || argc > 4) {
        std::fprintf(stderr,
            "Usage: %s <file.raw> [rate] [channels]\n"
            "  file.raw  raw signed 16-bit little-endian PCM bytes\n"
            "  rate      playback sample rate in Hz (default 48000)\n"
            "  channels  1 = mono (default), 2 = stereo interleaved L,R,L,R...\n",
            argv[0]);
        std::exit(1);
    }
    *datafile = argv[1];
    if (argc >= 3 && !parse_positive(argv[2], rate)) {
        std::fprintf(stderr, "error: rate '%s' is not a positive integer\n", argv[2]);
        std::exit(1);
    }
    if (argc == 4 && (!parse_positive(argv[3], channels) ||
                      (*channels != 1 && *channels != 2))) {
        std::fprintf(stderr, "error: channels '%s' must be 1 or 2\n", argv[3]);
        std::exit(1);
    }
}

// Read the whole byte file into the sample buffer; exit nonzero if it is
// unreadable or empty.
static void load_samples(const char* datafile) {
    std::ifstream file(datafile, std::ios::binary);
    if (!file) {
        std::fprintf(stderr, "error: cannot open '%s'\n", datafile);
        std::exit(1);
    }
    g_samples.assign(std::istreambuf_iterator<char>(file),
                     std::istreambuf_iterator<char>());
    if (g_samples.empty()) {
        std::fprintf(stderr, "error: '%s' is empty\n", datafile);
        std::exit(1);
    }
    g_state.data = g_samples.data();
    g_state.size = g_samples.size();
}

// Open the default playback device: s16 samples at the requested rate and
// channel count, fed by data_callback. Exits nonzero on failure.
static void init_audio(long rate, long channels) {
    ma_device_config config = ma_device_config_init(ma_device_type_playback);
    config.playback.format = ma_format_s16;
    config.playback.channels = (ma_uint32)channels;
    config.sampleRate = (ma_uint32)rate;
    config.dataCallback = data_callback;
    config.pUserData = &g_state;

    if (ma_device_init(nullptr, &config, &g_device) != MA_SUCCESS) {
        std::fprintf(stderr, "error: failed to open playback device\n");
        std::exit(1);
    }
}

// Print what is about to play and start the device; from here the audio
// thread streams the sample buffer via data_callback.
static void play_sample(long rate, long channels) {
    const size_t frames = g_state.size / (sizeof(int16_t) * channels);
    std::printf("Playing %zu bytes: %zu frames at %ld Hz, %ld channel(s) (~%.2f s)\n",
                g_state.size, frames, rate, channels,
                (double)frames / (double)rate);

    if (ma_device_start(&g_device) != MA_SUCCESS) {
        std::fprintf(stderr, "error: failed to start playback device\n");
        ma_device_uninit(&g_device);
        std::exit(1);
    }
}

// Block until the callback has consumed the whole buffer, give the backend
// a moment to drain the last buffer, then close the device.
// No need to change this
static void wait_for_audio() {
    while (!g_state.done.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds( kPollIntervalMs ));
    }
    // Give the backend a moment to drain the last buffer before closing.
    std::this_thread::sleep_for(std::chrono::milliseconds( kDrainMs ));
    ma_device_uninit(&g_device);
}

// main program
// parse commandline arguments
// load samples, replace with your 'generate samples' code
// init audio
// play sample
// wait for audio to finish
// done
int main(int argc, char** argv) {
    long rate = 0;
    long channels = 0;
    const char* datafile = nullptr;

    read_arguments(argc, argv, &rate, &channels, &datafile);
    load_samples(datafile);
    init_audio(rate, channels);
    play_sample(rate, channels);
    wait_for_audio();
    return 0;
}
