#pragma once
#include <stdbool.h>
#include "esp_err.h"
#include "modem_image.h"
#include "voice_protocol.h"

typedef enum { CONTENT_TEXT, CONTENT_IMAGE, CONTENT_AUDIO, CONTENT_COUNT } content_type_t;
typedef struct {
    uint32_t epoch, image_version, image_rows, image_bad_rows, image_completed;
    uint32_t voice_rate, voice_packets, voice_frames, voice_crc_errors, voice_resyncs;
    uint32_t encoded_frames, decoded_frames, concealed_frames, lost_packets, duplicates;
    uint32_t queue_drops, codec_max_us, output_errors;
    uint32_t voice_tx_packets, voice_tx_bytes, voice_tx_pending;
    uint8_t last_voice_frame[VOICE_FRAME_BYTES];
    bool codec_ready, draining;
} content_status_t;
typedef struct { voice_packet_t packet; size_t offset; uint32_t epoch; } voice_tx_stream_t;

const char *content_name(content_type_t type);
esp_err_t content_service_init(void);
/* Control and microphone capture belong to audio_task; codec_task owns Codec2. */
void content_begin(content_type_t type, bool transmit, bool active, uint32_t rate, uint32_t epoch);
void content_capture(const int16_t *pcm48k, size_t count);
void content_end_capture(void);
bool content_receive(const uint8_t *data, size_t len);
void content_get_status(content_status_t *status);
void content_copy_image(uint8_t *pixels, content_status_t *status);
int content_voice_tx_fetch(void *stream); /* -1 idle, -2 producer finished */
/* Called only by tx_task, the sole owner of the audio output. */
bool content_play_audio(void);
