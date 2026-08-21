#ifndef VAD_H
#define VAD_H

#include <Arduino.h>
#include <stdint.h>

/// Emitted for every PCM frame the gate lets through, in capture order.
///
/// @param pcm            OPUS_FRAME_SAMPLES samples of 16-bit mono PCM.
/// @param samples        Always OPUS_FRAME_SAMPLES.
/// @param age_ms         How long ago this frame was captured. Non-zero only
///                       while the pre-roll backlog is draining, so the caller
///                       can timestamp a session at the true speech onset
///                       rather than at the moment the gate tripped.
/// @param session_start  True on the first frame of a contiguous open period.
typedef void (*vad_frame_handler)(const int16_t *pcm, size_t samples, uint32_t age_ms, bool session_start);

/// Allocate the pre-roll ring (PSRAM). Safe to call twice.
bool vad_init();

void vad_set_callback(vad_frame_handler callback);

/// Feed exactly OPUS_FRAME_SAMPLES samples. Invokes the callback zero or more
/// times: zero while the gate is closed, once while it is open, and
/// VAD_PREROLL_FRAMES + 1 times on the frame that opens it.
void vad_process_frame(const int16_t *pcm);

/// True while the gate is open (including the hangover tail).
bool vad_is_open();

/// Most recent frame energy and adapted noise floor, for logging/tuning.
float vad_last_rms();
float vad_noise_floor();

/// Close the gate and drop the pre-roll. Used when the audio path is torn down.
void vad_reset();

#endif // VAD_H
