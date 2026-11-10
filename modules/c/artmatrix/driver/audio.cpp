#include <math.h>

#include "audio.hpp"

namespace pimoroni {

static float clamp01(float value) {
    return value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
}

// How far to move towards a target this capture, for an ease over `seconds`.
static float ease(float seconds) {
    const float step = AUDIO_CAPTURE_SECONDS / seconds;
    return step > 1.0f ? 1.0f : step;
}

static float log_scale(float value, float quiet, float loud) {
    return clamp01((logf(value + 1.0f) - logf(quiet)) / (logf(loud) - logf(quiet)));
}

Audio::Audio() {
    const float pi = 3.14159265f;
    for(uint32_t i = 0u; i < AUDIO_FFT_MAX; i++) {
        hann[i] = (uint16_t)(128.0f - 128.0f * cosf(2.0f * pi * i / AUDIO_FFT_MAX));
    }
    for(uint32_t i = 0u; i < AUDIO_FFT_MAX / 2; i++) {
        cos_table[i] = (int16_t)(cosf(2.0f * pi * i / AUDIO_FFT_MAX) * 16384.0f);
        sin_table[i] = (int16_t)(sinf(2.0f * pi * i / AUDIO_FFT_MAX) * 16384.0f);
    }
    for(uint32_t i = 0u; i < AUDIO_FFT_MAX; i++) {
        uint32_t flipped = 0u;
        for(uint32_t bit = 0u; bit < 9u; bit++) flipped |= ((i >> bit) & 1u) << (8u - bit);
        reversed_512[i] = flipped;
        if(i < AUDIO_CAPTURE) reversed_128[i] = flipped >> 2;
    }

    // log-spaced band tops over bins 1-64, never less than one bin a band
    uint32_t last = 0u;
    for(uint32_t band = 0u; band < AUDIO_BANDS; band++) {
        uint32_t top = (uint32_t)roundf(powf(64.0f, (band + 1.0f) / AUDIO_BANDS));
        if(top <= last) top = last + 1u;
        band_last[band] = top;
        last = top;
    }
}

/*
Hann window, radix-2, the same fixed point as the viper version the constants were
tuned on: the window is 0-256 then >>4, twiddles x16384, and every stage halves so
nothing outgrows 32 bits. Leaves the magnitudes of bins 0 to size/2 in real[],
roughly, as the bigger part plus half the smaller.
*/
void Audio::fft(const uint16_t *samples, uint32_t size) {
    const uint32_t step = AUDIO_FFT_MAX / size;
    const uint16_t *reversed = size == AUDIO_CAPTURE ? reversed_128 : reversed_512;

    int32_t total = 0;
    for(uint32_t i = 0u; i < size; i++) total += samples[i];
    const int32_t mean = total / (int32_t)size;

    for(uint32_t i = 0u; i < size; i++) {
        const uint32_t slot = reversed[i];
        real[slot] = ((int32_t)samples[i] - mean) * (int32_t)hann[i * step] >> 4;
        imag[slot] = 0;
    }

    for(uint32_t span = 2u; span <= size; span <<= 1) {
        const uint32_t half = span >> 1;
        const uint32_t stride = AUDIO_FFT_MAX / span;
        for(uint32_t start = 0u; start < size; start += span) {
            for(uint32_t pair = 0u; pair < half; pair++) {
                const int32_t cos = cos_table[pair * stride];
                const int32_t sin = sin_table[pair * stride];
                const uint32_t top = start + pair;
                const uint32_t bottom = top + half;
                const int32_t turned_real = (real[bottom] * cos + imag[bottom] * sin) >> 14;
                const int32_t turned_imag = (imag[bottom] * cos - real[bottom] * sin) >> 14;
                const int32_t top_real = real[top];
                const int32_t top_imag = imag[top];
                real[top] = (top_real + turned_real) >> 1;
                imag[top] = (top_imag + turned_imag) >> 1;
                real[bottom] = (top_real - turned_real) >> 1;
                imag[bottom] = (top_imag - turned_imag) >> 1;
            }
        }
    }

    for(uint32_t i = 0u; i <= size / 2; i++) {
        const int32_t a = real[i] < 0 ? -real[i] : real[i];
        const int32_t b = imag[i] < 0 ? -imag[i] : imag[i];
        real[i] = a > b ? a + (b >> 1) : b + (a >> 1);
    }
}

float Audio::seconds_since(uint32_t capture) {
    return (clock - capture) * AUDIO_CAPTURE_SECONDS;
}

void Audio::hit(AudioHit &which, float strength) {
    which.count = (which.count + 1u) & 0xfffffu;
    which.strength = strength;
    which.last = clock;
}

void Audio::skip(uint32_t captures) {
    clock += captures;
    joined_count = 0u;
}

float Audio::phase(void) {
    if(bpm == 0.0f) return 0.0f;
    const float beats = seconds_since(beat.last) * bpm / 60.0f;
    return beats - floorf(beats);
}

void Audio::feed(const uint16_t *capture) {
    clock++;
    const float dt = AUDIO_CAPTURE_SECONDS;

    fft(capture, AUDIO_CAPTURE);
    float overall = 0.0f;
    float high = 0.0f;
    float weighted = 0.0f;
    for(uint32_t bin = 1u; bin <= AUDIO_CAPTURE / 2; bin++) {
        const float magnitude = (float)real[bin];
        overall += magnitude;
        weighted += magnitude * bin;
        if(bin >= AUDIO_BEAT_LOW_BIN) high += magnitude;
    }

    // loudness, this capture's and eased
    const float loudness = log_scale(overall, AUDIO_QUIET, AUDIO_LOUD);
    level += (loudness - level) * ease(loudness > level ? AUDIO_LEVEL_RISE : AUDIO_LEVEL_FALL);
    const bool music = level > AUDIO_GATE;

    if(music) {
        if(!active) heard = 0.0f;
        since_loud = 0.0f;
    } else if(since_loud < AUDIO_ACTIVE_HOLD) {
        since_loud += dt;
        if(since_loud >= AUDIO_ACTIVE_HOLD) {
            // the music stopped, so the next song starts its tempo afresh
            gap_count = 0u;
            beat_before = false;
            bpm = 0.0f;
        }
    }
    active = since_loud < AUDIO_ACTIVE_HOLD;

    heard += dt;
    level_short += (level - level_short) * ease(heard < AUDIO_INTENSITY_SHORT ? heard : AUDIO_INTENSITY_SHORT);
    level_long += (level - level_long) * ease(heard < AUDIO_INTENSITY_LONG ? heard : AUDIO_INTENSITY_LONG);
    intensity = clamp01(0.5f + (level_short - level_long) / (2.0f * AUDIO_INTENSITY_SPAN));

    // only while this capture's loud enough, so a fade into hiss doesn't drag it up
    if(loudness > AUDIO_GATE) {
        float centre = weighted / overall;
        if(centre < AUDIO_TONE_LOW_BIN) centre = AUDIO_TONE_LOW_BIN;
        const float aim = clamp01(logf(centre / AUDIO_TONE_LOW_BIN) / logf(AUDIO_TONE_HIGH_BIN / AUDIO_TONE_LOW_BIN));
        tone += (aim - tone) * ease(AUDIO_TONE_EASE);
    }

    float rises = 0.0f;
    uint32_t first = 1u;
    for(uint32_t band = 0u; band < AUDIO_BANDS; band++) {
        float sum = 0.0f;
        for(uint32_t bin = first; bin <= band_last[band]; bin++) sum += (float)real[bin];
        const float value = log_scale(sum / (band_last[band] - first + 1u), AUDIO_BAND_QUIET, AUDIO_BAND_LOUD[band]);
        if(value > raw_bands[band]) rises += value - raw_bands[band];
        raw_bands[band] = value;
        bands[band] += (value - bands[band]) * ease(value > bands[band] ? AUDIO_BAND_RISE : AUDIO_BAND_FALL);
        first = band_last[band] + 1u;
    }
    const float busy = clamp01(rises / AUDIO_FLUX_FULL);
    flux += (busy - flux) * ease(AUDIO_FLUX_EASE);

    // beat, off the loudest of the last few captures' 4kHz+ energy
    high_recent[clock % AUDIO_BEAT_PEAK_CAPTURES] = high;
    float peak = 0.0f;
    for(uint32_t i = 0u; i < AUDIO_BEAT_PEAK_CAPTURES; i++) {
        if(high_recent[i] > peak) peak = high_recent[i];
    }
    const float gap = seconds_since(beat.last);
    const bool beaten = music && beat_average > 0.0f && peak > beat_average * AUDIO_BEAT_RISE
                        && gap > AUDIO_BEAT_GAP;
    if(beaten) hit(beat, clamp01(logf(peak / beat_average) / logf(AUDIO_HIT_FULL)));

    // the first beat after a quiet spell has no gap worth timing
    if(beaten && !beat_before) beat_before = true;
    else if(beaten) {
        float tempo = 60.0f / gap;
        while(tempo < AUDIO_BPM_LOW) tempo *= 2.0f;
        while(tempo > AUDIO_BPM_HIGH) tempo *= 0.5f;
        gaps[gap_count % AUDIO_BPM_GAPS] = tempo;
        gap_count++;

        // median of the gaps we have, then the mean of those that agree with it
        const uint32_t n = gap_count < AUDIO_BPM_GAPS ? gap_count : AUDIO_BPM_GAPS;
        float sorted[AUDIO_BPM_GAPS];
        for(uint32_t i = 0u; i < n; i++) {
            uint32_t j = i;
            for(; j > 0u && sorted[j - 1u] > gaps[i]; j--) sorted[j] = sorted[j - 1u];
            sorted[j] = gaps[i];
        }
        const float median = n & 1u ? sorted[n / 2u] : (sorted[n / 2u - 1u] + sorted[n / 2u]) * 0.5f;
        float agreeing = 0.0f;
        uint32_t agree = 0u;
        for(uint32_t i = 0u; i < n; i++) {
            if(fabsf(sorted[i] - median) < median * AUDIO_BPM_SPREAD) {
                agreeing += sorted[i];
                agree++;
            }
        }
        bpm = agree >= AUDIO_BPM_AGREE ? agreeing / agree : 0.0f;
    }
    beat_average += (peak - beat_average) * ease(AUDIO_HIT_AVERAGE);

    if(clap_pending) {
        clap_for += dt;
        if(loudness > AUDIO_GATE) {
            clap_after = 0.0f;
            if(loudness > clap_peak) clap_peak = loudness;
            if(clap_for > AUDIO_CLAP_LENGTH) {
                clap_pending = false;
                quiet_for = 0.0f;
            }
        } else {
            clap_after += dt;
            if(clap_after >= AUDIO_CLAP_AFTER) {
                hit(clap, clap_peak);
                clap_pending = false;
                quiet_for = clap_after;
            }
        }
    } else if(loudness >= AUDIO_CLAP_JUMP && quiet_for >= AUDIO_CLAP_QUIET) {
        clap_pending = true;
        clap_peak = loudness;
        clap_for = 0.0f;
        clap_after = 0.0f;
    } else if(loudness > AUDIO_GATE) {
        quiet_for = 0.0f;
    } else {
        quiet_for += dt;
    }

    int32_t total = 0;
    for(uint32_t i = 0u; i < AUDIO_CAPTURE; i++) total += capture[i];
    const float mean = (float)total / AUDIO_CAPTURE;
    for(uint32_t i = 0u; i < AUDIO_WAVE; i++) {
        const float sample = (capture[i * 2u] + capture[i * 2u + 1u]) * 0.5f - mean;
        const float scaled = sample / AUDIO_WAVE_FULL;
        wave[i] = scaled < -1.0f ? -1.0f : (scaled > 1.0f ? 1.0f : scaled);
    }

    // kick, once four unbroken captures are in, every fourth one
    const uint32_t slot = clock % AUDIO_KICK_CAPTURES;
    for(uint32_t i = 0u; i < AUDIO_CAPTURE; i++) joined[slot * AUDIO_CAPTURE + i] = capture[i];
    if(joined_count < AUDIO_KICK_CAPTURES) joined_count++;
    if(slot == AUDIO_KICK_CAPTURES - 1u && joined_count == AUDIO_KICK_CAPTURES) {
        fft(joined, AUDIO_FFT_MAX);
        float bass = 0.0f;
        for(uint32_t bin = AUDIO_KICK_LOW_BIN; bin <= AUDIO_KICK_HIGH_BIN; bin++) bass += (float)real[bin];
        if(music && kick_average > 0.0f && bass > kick_average * AUDIO_KICK_RISE
           && seconds_since(kick.last) > AUDIO_KICK_GAP) {
            hit(kick, clamp01(logf(bass / kick_average) / logf(AUDIO_HIT_FULL)));
        }
        kick_average += (bass - kick_average) * ease(AUDIO_HIT_AVERAGE / AUDIO_KICK_CAPTURES);
    }
}

}
