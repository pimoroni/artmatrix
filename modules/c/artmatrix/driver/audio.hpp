#pragma once

#include <stdint.h>

// Sound analysis for the SENS header's mic, fed one 4ms capture at a time.
// Plain C++ with no pico-sdk, so it builds on a desktop against a recording.
namespace pimoroni {

/*
Tuned on a MAX9814 with its gain pin at 40dB: hiss is ~7 counts, music ~97.
Loudness is the sum of the 128-point FFT's 64 magnitudes on a log scale from
AUDIO_QUIET to AUDIO_LOUD. Everything that only means something with music
playing waits for the eased level to pass AUDIO_GATE, which is the room's hiss
spikes kept out. A quiet room reads 0, and a quiet intro with a clear beat ~0.2.
*/
const uint32_t AUDIO_CAPTURE = 128;               // samples, 32kHz
constexpr float AUDIO_CAPTURE_SECONDS = 1.0f / 247.0f; // measured, captures sit a little apart
constexpr float AUDIO_QUIET = 600.0f;
constexpr float AUDIO_LOUD = 5000.0f;
constexpr float AUDIO_LEVEL_RISE = 0.3f;          // seconds
constexpr float AUDIO_LEVEL_FALL = 1.0f;
constexpr float AUDIO_GATE = 0.12f;

/*
A beat is the 4kHz+ energy (bins 16-64) over AUDIO_BEAT_RISE times its recent
average. A 4ms window is too short to see bass; the hats and snares carry the
beat. The loudest of the last four captures is what's tested, which on a 126bpm
recording put as many beats on the grid as the per-frame viper version did.
Hit strength is log(ratio) / log(AUDIO_HIT_FULL): music's beats ran 1.5x-13x,
so a linear scale would read 1 on most of them.
*/
const uint32_t AUDIO_BEAT_LOW_BIN = 16;
const uint32_t AUDIO_BEAT_PEAK_CAPTURES = 4;
constexpr float AUDIO_BEAT_RISE = 1.5f;
constexpr float AUDIO_BEAT_GAP = 0.2f;            // seconds, so one hit isn't two
constexpr float AUDIO_HIT_AVERAGE = 0.5f;         // seconds
constexpr float AUDIO_HIT_FULL = 10.0f;

// The kick drum, from four captures joined into a 512-point FFT (62.5Hz bins),
// bins 1-3. Only every fourth capture, so up to 16ms late.
const uint32_t AUDIO_KICK_CAPTURES = 4;
const uint32_t AUDIO_KICK_LOW_BIN = 1;
const uint32_t AUDIO_KICK_HIGH_BIN = 3;
constexpr float AUDIO_KICK_RISE = 1.5f;
constexpr float AUDIO_KICK_GAP = 0.25f;

/*
A clap is one capture's loudness (not eased) jumping to AUDIO_CLAP_JUMP after
AUDIO_CLAP_QUIET seconds under the gate, gone again within AUDIO_CLAP_LENGTH, then
AUDIO_CLAP_AFTER of quiet. It's reported once that quiet has passed, so it's late
by that much, and anything that keeps going isn't a clap.
*/
constexpr float AUDIO_CLAP_JUMP = 0.5f;
constexpr float AUDIO_CLAP_QUIET = 1.0f;
constexpr float AUDIO_CLAP_LENGTH = 0.15f;
constexpr float AUDIO_CLAP_AFTER = 0.3f;

// Tone is where the sound's centred, bins 7-22 on a log scale. Music sat
// between ~8 and ~20, hiss alone at ~33.
constexpr float AUDIO_TONE_LOW_BIN = 7.0f;
constexpr float AUDIO_TONE_HIGH_BIN = 22.0f;
constexpr float AUDIO_TONE_EASE = 0.5f;

/*
16 bands, 250Hz-16kHz log-spaced, the lowest eight one bin each. Each band is its
bins' mean magnitude on a log scale from AUDIO_BAND_QUIET, just over the hiss, to
its own loud point. Those are the 126bpm recording's top 1%: flat to 2kHz, then
falling with the square of the frequency, as the mic hears little up there.
*/
const uint32_t AUDIO_BANDS = 16;
constexpr float AUDIO_BAND_QUIET = 8.0f;
static constexpr float AUDIO_BAND_LOUD[AUDIO_BANDS] = {
    600.0f, 600.0f, 600.0f, 600.0f, 600.0f, 600.0f, 600.0f, 600.0f,
    425.0f, 267.0f, 160.0f, 91.0f, 55.0f, 33.0f, 24.0f, 24.0f
};
constexpr float AUDIO_BAND_RISE = 0.03f;
constexpr float AUDIO_BAND_FALL = 0.4f;

// Flux is the bands' rises since the last capture, summed, over AUDIO_FLUX_FULL.
// The recording's music came to ~0.8 a capture.
constexpr float AUDIO_FLUX_FULL = 1.6f;
constexpr float AUDIO_FLUX_EASE = 0.3f;

constexpr float AUDIO_ACTIVE_HOLD = 2.0f;         // seconds

// Intensity is the level's 5s average against its 30s one, 0.5 when they match,
// 0 or 1 when they're AUDIO_INTENSITY_SPAN apart. Both average over the time since
// the music started until they're that long, so it starts from the middle.
constexpr float AUDIO_INTENSITY_SHORT = 5.0f;
constexpr float AUDIO_INTENSITY_LONG = 30.0f;
constexpr float AUDIO_INTENSITY_SPAN = 0.2f;

// Tempo from the last AUDIO_BPM_GAPS beat gaps, each folded into 70-140bpm to cut
// doubling and halving. Locks once AUDIO_BPM_AGREE are within 15% of the median.
const uint32_t AUDIO_BPM_GAPS = 8;
const uint32_t AUDIO_BPM_AGREE = 4;
constexpr float AUDIO_BPM_LOW = 70.0f;
constexpr float AUDIO_BPM_HIGH = 140.0f;
constexpr float AUDIO_BPM_SPREAD = 0.15f;

// The newest capture with pairs averaged, ±AUDIO_WAVE_FULL counts to ±1.
const uint32_t AUDIO_WAVE = 64;
constexpr float AUDIO_WAVE_FULL = 400.0f;

const uint32_t AUDIO_FFT_MAX = AUDIO_CAPTURE * AUDIO_KICK_CAPTURES;

struct AudioHit {
    uint32_t count = 0;                           // wraps at 2^20, so it stays exact in a float
    float strength = 0.0f;
    uint32_t last = 0;                            // capture it fired on
};

class Audio {
  public:
    Audio();
    void feed(const uint16_t *capture);
    void skip(uint32_t captures);                 // captures that came and went unheard

    float level = 0.0f;
    float tone = 0.0f;
    float flux = 0.0f;
    float intensity = 0.5f;
    bool active = false;
    float bpm = 0.0f;
    float bands[AUDIO_BANDS] = {};
    float wave[AUDIO_WAVE] = {};
    AudioHit beat;
    AudioHit kick;
    AudioHit clap;

    float phase(void);

  private:
    void fft(const uint16_t *samples, uint32_t size);
    void hit(AudioHit &which, float strength);
    float seconds_since(uint32_t capture);

    // Tables for the 512-point FFT; the 128-point one reads every fourth entry.
    uint16_t hann[AUDIO_FFT_MAX];
    int16_t cos_table[AUDIO_FFT_MAX / 2];
    int16_t sin_table[AUDIO_FFT_MAX / 2];
    uint16_t reversed_128[AUDIO_CAPTURE];
    uint16_t reversed_512[AUDIO_FFT_MAX];
    uint8_t band_last[AUDIO_BANDS];
    int32_t real[AUDIO_FFT_MAX];
    int32_t imag[AUDIO_FFT_MAX];

    uint16_t joined[AUDIO_FFT_MAX];               // the last four captures, for the kick
    uint32_t clock = 0;                           // captures heard or skipped
    uint32_t joined_count = 0;                    // unbroken captures in joined, up to four

    float high_recent[AUDIO_BEAT_PEAK_CAPTURES] = {};
    float beat_average = 0.0f;
    float kick_average = 0.0f;
    float raw_bands[AUDIO_BANDS] = {};
    float level_short = 0.0f;
    float level_long = 0.0f;
    float since_loud = AUDIO_ACTIVE_HOLD;
    float heard = 0.0f;                           // seconds since the music started

    float quiet_for = 0.0f;
    bool clap_pending = false;
    float clap_peak = 0.0f;
    float clap_for = 0.0f;
    float clap_after = 0.0f;

    float gaps[AUDIO_BPM_GAPS] = {};
    uint32_t gap_count = 0;
    bool beat_before = false;                     // since the music last stopped
};

}
