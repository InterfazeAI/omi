#ifndef OPUS_ENCODER_H
#define OPUS_ENCODER_H

#include <Arduino.h>
#include <stdint.h>

/// Opus encoder for the Omi `opusFS320` codec (id 21): 16 kHz mono, 320-sample
/// (20 ms) frames.
///
/// PCM arrives from the mic in arbitrary chunk sizes and is re-framed here.
/// Framing is exposed rather than driven by a callback because the voice
/// activity gate sits between the two: PCM frames are popped, offered to the
/// gate, and only the ones it lets through are encoded.

bool opus_encoder_init();

/// Push mic samples into the PCM ring. Oldest samples are dropped on overflow.
int opus_receive_pcm(const int16_t *data, size_t samples);

/// Samples currently buffered and not yet popped.
size_t opus_pcm_available();

/// Pop exactly OPUS_FRAME_SAMPLES samples into `out`. False when the ring holds
/// less than a full frame.
bool opus_pcm_pop_frame(int16_t *out);

/// Encode one OPUS_FRAME_SAMPLES frame. Returns the encoded length in bytes
/// (1-2 bytes for a DTX frame) or -1 on error. The bytes live in the encoder's
/// output buffer, valid until the next call.
int opus_encode_frame(const int16_t *pcm);

const uint8_t *opus_frame_data();

/// Codec id advertised on 19B10002.
uint8_t opus_get_codec_id();

#endif // OPUS_ENCODER_H
