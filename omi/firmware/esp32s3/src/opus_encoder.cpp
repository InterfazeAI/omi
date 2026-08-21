#include "opus_encoder.h"

#include <esp_heap_caps.h>
#include <opus.h>

#include "config.h"

static OpusEncoder *encoder = nullptr;

// PCM ring between the mic and the framing step, allocated in PSRAM.
static int16_t *pcm_ring_buffer = nullptr;
static volatile size_t ring_write_pos = 0;
static volatile size_t ring_read_pos = 0;

static uint8_t *opus_output_buffer = nullptr;

bool opus_encoder_init()
{
    if (encoder != nullptr) {
        return true;
    }

    pcm_ring_buffer = (int16_t *) heap_caps_malloc(AUDIO_RING_BUFFER_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (pcm_ring_buffer == nullptr) {
        Serial.println("Opus: failed to allocate PCM ring buffer");
        return false;
    }

    opus_output_buffer = (uint8_t *) heap_caps_malloc(OPUS_OUTPUT_MAX_BYTES, MALLOC_CAP_SPIRAM);
    if (opus_output_buffer == nullptr) {
        Serial.println("Opus: failed to allocate output buffer");
        heap_caps_free(pcm_ring_buffer);
        pcm_ring_buffer = nullptr;
        return false;
    }

    int error = OPUS_OK;
    encoder = opus_encoder_create(MIC_SAMPLE_RATE, 1, OPUS_APPLICATION_VOIP, &error);
    if (error != OPUS_OK || encoder == nullptr) {
        Serial.printf("Opus: encoder create failed: %d\n", error);
        heap_caps_free(pcm_ring_buffer);
        heap_caps_free(opus_output_buffer);
        pcm_ring_buffer = nullptr;
        opus_output_buffer = nullptr;
        encoder = nullptr;
        return false;
    }

    opus_encoder_ctl(encoder, OPUS_SET_BITRATE(OPUS_BITRATE));
    opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(OPUS_COMPLEXITY));
    opus_encoder_ctl(encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    opus_encoder_ctl(encoder, OPUS_SET_VBR(OPUS_VBR));
    opus_encoder_ctl(encoder, OPUS_SET_VBR_CONSTRAINT(0));
    opus_encoder_ctl(encoder, OPUS_SET_LSB_DEPTH(16));
    // DTX earns its keep inside an open gate: word gaps and the hangover tail
    // collapse to 1-2 byte frames while the 20 ms cadence — and therefore the
    // reconstructed timeline — stays exact.
    opus_encoder_ctl(encoder, OPUS_SET_DTX(OPUS_DTX));
    opus_encoder_ctl(encoder, OPUS_SET_INBAND_FEC(0));
    opus_encoder_ctl(encoder, OPUS_SET_PACKET_LOSS_PERC(0));

    ring_write_pos = 0;
    ring_read_pos = 0;

    Serial.printf("Opus: %d Hz, %d bps, %d-sample frames, DTX %s\n",
                  MIC_SAMPLE_RATE,
                  OPUS_BITRATE,
                  OPUS_FRAME_SAMPLES,
                  OPUS_DTX ? "on" : "off");
    return true;
}

int opus_receive_pcm(const int16_t *data, size_t samples)
{
    if (pcm_ring_buffer == nullptr || data == nullptr) {
        return -1;
    }
    for (size_t i = 0; i < samples; i++) {
        const size_t next_write = (ring_write_pos + 1) % AUDIO_RING_BUFFER_SAMPLES;
        if (next_write == ring_read_pos) {
            ring_read_pos = (ring_read_pos + 1) % AUDIO_RING_BUFFER_SAMPLES;
        }
        pcm_ring_buffer[ring_write_pos] = data[i];
        ring_write_pos = next_write;
    }
    return 0;
}

size_t opus_pcm_available()
{
    const size_t w = ring_write_pos;
    const size_t r = ring_read_pos;
    return (w >= r) ? (w - r) : (AUDIO_RING_BUFFER_SAMPLES - r + w);
}

bool opus_pcm_pop_frame(int16_t *out)
{
    if (pcm_ring_buffer == nullptr || out == nullptr || opus_pcm_available() < OPUS_FRAME_SAMPLES) {
        return false;
    }
    for (size_t i = 0; i < OPUS_FRAME_SAMPLES; i++) {
        out[i] = pcm_ring_buffer[ring_read_pos];
        ring_read_pos = (ring_read_pos + 1) % AUDIO_RING_BUFFER_SAMPLES;
    }
    return true;
}

int opus_encode_frame(const int16_t *pcm)
{
    if (encoder == nullptr || opus_output_buffer == nullptr || pcm == nullptr) {
        return -1;
    }

    const opus_int32 encoded = opus_encode(encoder, pcm, OPUS_FRAME_SAMPLES, opus_output_buffer, OPUS_OUTPUT_MAX_BYTES);
    if (encoded < 0) {
        Serial.printf("Opus: encode error %d\n", (int) encoded);
        return -1;
    }
    return (int) encoded;
}

const uint8_t *opus_frame_data()
{
    return opus_output_buffer;
}

uint8_t opus_get_codec_id()
{
    return AUDIO_CODEC_ID;
}
