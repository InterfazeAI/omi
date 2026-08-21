#include "vad.h"

#include <esp_heap_caps.h>
#include <math.h>

#include "config.h"

static vad_frame_handler frame_callback = nullptr;

// Pre-roll ring: VAD_PREROLL_FRAMES frames of PCM kept so that the syllable that
// tripped the gate is not clipped off the front of the recording.
static int16_t *preroll = nullptr;
static uint16_t preroll_write = 0; // next slot to write
static uint16_t preroll_count = 0; // valid frames currently buffered

static bool gate_open = false;
static uint16_t hangover_left = 0;
static float noise_floor = VAD_FLOOR_INIT;
static float last_rms = 0.0f;

bool vad_init()
{
    if (preroll != nullptr) {
        return true;
    }

    const size_t bytes = (size_t) VAD_PREROLL_FRAMES * OPUS_FRAME_SAMPLES * sizeof(int16_t);
    preroll = (int16_t *) heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    if (preroll == nullptr) {
        preroll = (int16_t *) malloc(bytes);
    }
    if (preroll == nullptr) {
        Serial.println("VAD: failed to allocate pre-roll buffer");
        return false;
    }

    vad_reset();
    Serial.printf("VAD: pre-roll %d frames (%d ms), hangover %d ms\n",
                  (int) VAD_PREROLL_FRAMES,
                  (int) VAD_PREROLL_MS,
                  (int) VAD_HANGOVER_MS);
    return true;
}

void vad_set_callback(vad_frame_handler callback)
{
    frame_callback = callback;
}

void vad_reset()
{
    gate_open = false;
    hangover_left = 0;
    preroll_write = 0;
    preroll_count = 0;
    noise_floor = VAD_FLOOR_INIT;
    last_rms = 0.0f;
}

bool vad_is_open()
{
    return gate_open;
}

float vad_last_rms()
{
    return last_rms;
}

float vad_noise_floor()
{
    return noise_floor;
}

static float frame_rms(const int16_t *pcm)
{
    uint64_t acc = 0;
    for (size_t i = 0; i < OPUS_FRAME_SAMPLES; i++) {
        const int32_t s = pcm[i];
        acc += (uint64_t) (s * s);
    }
    return sqrtf((float) acc / (float) OPUS_FRAME_SAMPLES);
}

static void preroll_push(const int16_t *pcm)
{
    memcpy(preroll + (size_t) preroll_write * OPUS_FRAME_SAMPLES, pcm, OPUS_FRAME_SAMPLES * sizeof(int16_t));
    preroll_write = (uint16_t) ((preroll_write + 1) % VAD_PREROLL_FRAMES);
    if (preroll_count < VAD_PREROLL_FRAMES) {
        preroll_count++;
    }
}

// Emit the buffered pre-roll oldest-first, then clear it. Ages are reported
// relative to the frame currently being processed.
static void preroll_flush()
{
    if (frame_callback == nullptr) {
        preroll_count = 0;
        return;
    }

    const uint16_t count = preroll_count;
    for (uint16_t i = 0; i < count; i++) {
        const uint16_t slot = (uint16_t) ((preroll_write + VAD_PREROLL_FRAMES - count + i) % VAD_PREROLL_FRAMES);
        const uint32_t age_ms = (uint32_t) (count - i) * OPUS_FRAME_MS;
        frame_callback(preroll + (size_t) slot * OPUS_FRAME_SAMPLES, OPUS_FRAME_SAMPLES, age_ms, i == 0);
    }
    preroll_count = 0;
}

void vad_process_frame(const int16_t *pcm)
{
    if (preroll == nullptr || pcm == nullptr) {
        return;
    }

    last_rms = frame_rms(pcm);

    // Adapt the noise floor asymmetrically: drop quickly towards a quieter room,
    // creep upwards so sustained speech cannot drag the floor up over itself.
    const float rate = (last_rms < noise_floor) ? VAD_FLOOR_FALL : VAD_FLOOR_RISE;
    noise_floor += (last_rms - noise_floor) * rate;
    if (noise_floor < VAD_FLOOR_MIN) {
        noise_floor = VAD_FLOOR_MIN;
    }
    if (noise_floor > VAD_FLOOR_MAX) {
        noise_floor = VAD_FLOOR_MAX;
    }

    const bool speech = gate_open ? (last_rms > noise_floor * VAD_CLOSE_FACTOR + VAD_CLOSE_BIAS)
                                  : (last_rms > noise_floor * VAD_OPEN_FACTOR + VAD_OPEN_BIAS);

    if (!gate_open) {
        if (!speech) {
            preroll_push(pcm);
            return;
        }

        gate_open = true;
        hangover_left = VAD_HANGOVER_FRAMES;

        // preroll_flush emits the session-start frame; it only skips doing so
        // when the pre-roll is empty (the first frames after boot or reset).
        const bool had_preroll = preroll_count > 0;
        preroll_flush();
        if (frame_callback != nullptr) {
            frame_callback(pcm, OPUS_FRAME_SAMPLES, 0, !had_preroll);
        }
        return;
    }

    if (speech) {
        hangover_left = VAD_HANGOVER_FRAMES;
    } else if (hangover_left > 0) {
        hangover_left--;
    }

    if (frame_callback != nullptr) {
        frame_callback(pcm, OPUS_FRAME_SAMPLES, 0, false);
    }

    if (!speech && hangover_left == 0) {
        gate_open = false;
        preroll_write = 0;
        preroll_count = 0;
    }
}
