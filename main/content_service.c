#include "content_service.h"
#include <string.h>
#include "codec2.h"
#include "voice_playout.h"
#include "voice_resample.h"
#include "audio_input.h"
#include "transport.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

typedef struct { voice_packet_t packet; uint32_t epoch; } packet_item_t;
typedef struct { int16_t pcm[VOICE_FRAME_SAMPLES]; uint32_t epoch; bool end, valid; } pcm_item_t;
typedef struct { voice_record_t record; uint32_t epoch; } record_item_t;
typedef struct { content_type_t type; bool transmit, active; uint32_t epoch; } session_t;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static QueueHandle_t s_packets, s_input, s_records, s_output;
static session_t s_session;
static content_status_t s_status;
static bool s_encoder_ended;
static uint8_t s_pixels[MODEM_IMAGE_PIXEL_COUNT];
static modem_image_rx_parser_t s_image_rx;
static voice_wire_rx_t s_wire_rx;
static uint32_t s_sync_drops;
/* Capture state is exclusively owned by audio_task. */
static voice_downsample_t s_down;
static pcm_item_t s_capture;
static unsigned s_capture_used;

const char *content_name(content_type_t type)
{
    static const char *names[] = {"Text", "Image", "Audio"};
    return (unsigned)type < CONTENT_COUNT ? names[type] : "?";
}
static session_t session_get(void)
{
    portENTER_CRITICAL(&s_lock); session_t s = s_session; portEXIT_CRITICAL(&s_lock); return s;
}
static void count_drop(void)
{
    portENTER_CRITICAL(&s_lock); ++s_status.queue_drops; portEXIT_CRITICAL(&s_lock);
}
void content_get_status(content_status_t *status)
{
    portENTER_CRITICAL(&s_lock); *status = s_status; portEXIT_CRITICAL(&s_lock);
    status->voice_tx_pending = s_packets ? uxQueueMessagesWaiting(s_packets) : 0;
}
void content_copy_image(uint8_t *pixels, content_status_t *status)
{
    portENTER_CRITICAL(&s_lock);
    memcpy(pixels, s_pixels, sizeof(s_pixels)); *status = s_status;
    portEXIT_CRITICAL(&s_lock);
}
void content_begin(content_type_t type, bool transmit, bool active, uint32_t rate, uint32_t epoch)
{
    (void)rate;
    modem_image_rx_parser_init(&s_image_rx);
    memset(&s_wire_rx, 0, sizeof(s_wire_rx)); s_sync_drops = 0;
    memset(&s_down, 0, sizeof(s_down)); memset(&s_capture, 0, sizeof(s_capture)); s_capture_used = 0;
    xQueueReset(s_packets); xQueueReset(s_input); xQueueReset(s_records); xQueueReset(s_output);
    transport_begin_session(epoch);
    portENTER_CRITICAL(&s_lock);
    const bool ready = s_status.codec_ready;
    const uint32_t version = s_status.image_version + 1;
    memset(&s_status, 0, sizeof(s_status));
    s_status.codec_ready = ready; s_status.epoch = epoch;
    s_status.voice_rate = VOICE_RATE; s_status.image_version = version;
    s_session = (session_t){type, transmit, active, epoch}; s_encoder_ended = false;
    if (type == CONTENT_IMAGE && transmit) {
        memcpy(s_pixels, g_modem_test_image_rgb332, sizeof(s_pixels));
        s_status.image_rows = MODEM_IMAGE_HEIGHT;
    } else memset(s_pixels, 0, sizeof(s_pixels));
    portEXIT_CRITICAL(&s_lock);
}
static void image_start(void *user)
{
    (void)user;
    portENTER_CRITICAL(&s_lock);
    memset(s_pixels, 0, sizeof(s_pixels)); s_status.image_rows = 0; ++s_status.image_version;
    portEXIT_CRITICAL(&s_lock);
}
static void image_row(uint16_t row, const uint8_t *pixels, size_t count, uint16_t complete, void *user)
{
    (void)user;
    if (row >= MODEM_IMAGE_HEIGHT || count != MODEM_IMAGE_WIDTH) return;
    portENTER_CRITICAL(&s_lock);
    memcpy(s_pixels + row * MODEM_IMAGE_WIDTH, pixels, count);
    s_status.image_rows = complete; ++s_status.image_version;
    portEXIT_CRITICAL(&s_lock);
}
static void receive_voice(const voice_record_t *record, void *user)
{
    (void)user;
    record_item_t item = {.record = *record, .epoch = session_get().epoch};
    if (xQueueSend(s_records, &item, 0) != pdTRUE) count_drop();
    /* Export the self-describing CRC-checked wire packets, including END. */
    voice_packet_t packet;
    if (voice_encode(record, &packet)) transport_send(packet.data, packet.len);
    if (record->count) {
        portENTER_CRITICAL(&s_lock);
        memcpy(s_status.last_voice_frame, record->frames[record->count - 1], VOICE_FRAME_BYTES);
        portEXIT_CRITICAL(&s_lock);
    }
}
bool content_receive(const uint8_t *data, size_t len)
{
    const session_t s = session_get();
    if (s.transmit) return false;
    if (s.type == CONTENT_IMAGE) {
        modem_image_rx_parser_feed(&s_image_rx, (const char *)data, len, image_start, image_row, NULL);
        transport_send(data, len);
        portENTER_CRITICAL(&s_lock);
        s_status.image_bad_rows = s_image_rx.total_invalid_rows;
        s_status.image_completed = s_image_rx.completed_frames;
        portEXIT_CRITICAL(&s_lock);
    } else if (s.type == CONTENT_AUDIO) {
        const uint32_t packets = s_wire_rx.packets;
        voice_wire_feed(&s_wire_rx, data, len, receive_voice, NULL);
        if (packets != s_wire_rx.packets) s_sync_drops = s_wire_rx.sync_drops;
        const bool reacquire = s_wire_rx.sync_drops - s_sync_drops >= 128;
        portENTER_CRITICAL(&s_lock);
        s_status.voice_packets = s_wire_rx.packets; s_status.voice_frames = s_wire_rx.frames;
        s_status.voice_crc_errors = s_wire_rx.crc_errors;
        if (reacquire) ++s_status.voice_resyncs;
        portEXIT_CRITICAL(&s_lock);
        if (reacquire) { s_sync_drops = s_wire_rx.sync_drops; s_wire_rx.used = 0; }
        return reacquire;
    }
    return false;
}
void content_capture(const int16_t *pcm, size_t count)
{
    const session_t s = session_get();
    if (s.type != CONTENT_AUDIO || !s.transmit || !s.active) return;
    /* Bounded scratch, independent of caller block size. */
    while (count) {
        const size_t n = count > 256 ? 256 : count;
        int16_t speech[43];
        const size_t used = voice_downsample(&s_down, pcm, n, speech);
        pcm += n; count -= n;
        for (size_t i = 0; i < used; ++i) {
            s_capture.pcm[s_capture_used++] = speech[i];
            if (s_capture_used == VOICE_FRAME_SAMPLES) {
                s_capture.epoch = s.epoch; s_capture.valid = true; s_capture.end = false;
                if (xQueueSend(s_input, &s_capture, 0) != pdTRUE) count_drop();
                s_capture_used = 0;
            }
        }
    }
}
void content_end_capture(void)
{
    const session_t s = session_get();
    if (!s.active || s.type != CONTENT_AUDIO || !s.transmit) return;
    s_capture.epoch = s.epoch; s_capture.end = true; s_capture.valid = s_capture_used != 0;
    memset(s_capture.pcm + s_capture_used, 0, (VOICE_FRAME_SAMPLES - s_capture_used) * sizeof(int16_t));
    /* END must fit even after CPU overload. Drop the oldest captured frame. */
    if (xQueueSend(s_input, &s_capture, 0) != pdTRUE) {
        pcm_item_t old; (void)xQueueReceive(s_input, &old, 0); count_drop();
        if (xQueueSend(s_input, &s_capture, 0) != pdTRUE) count_drop();
    }
    s_capture_used = 0;
    portENTER_CRITICAL(&s_lock); s_session.active = false; s_status.draining = true; portEXIT_CRITICAL(&s_lock);
}
int content_voice_tx_fetch(void *user)
{
    voice_tx_stream_t *stream = user;
    if (stream->offset >= stream->packet.len) {
        packet_item_t item;
        while (xQueueReceive(s_packets, &item, 0) == pdTRUE) {
            if (item.epoch != stream->epoch) continue;
            stream->packet = item.packet; stream->offset = 0; break;
        }
    }
    if (stream->offset >= stream->packet.len) {
        portENTER_CRITICAL(&s_lock);
        const bool ended = s_encoder_ended && s_session.epoch == stream->epoch;
        portEXIT_CRITICAL(&s_lock);
        return ended ? -2 : -1;
    }
    const int byte = stream->packet.data[stream->offset++];
    if (stream->offset == stream->packet.len) {
        portENTER_CRITICAL(&s_lock);
        if (s_status.epoch == stream->epoch) {
            ++s_status.voice_tx_packets; s_status.voice_tx_bytes += stream->packet.len;
        }
        portEXIT_CRITICAL(&s_lock);
    }
    return byte;
}
static void enqueue_record(voice_record_t *r, uint32_t epoch)
{
    packet_item_t item = {.epoch = epoch};
    if (voice_encode(r, &item.packet) && session_get().epoch == epoch) {
        if (xQueueSend(s_packets, &item, 0) != pdTRUE) count_drop();
    }
    ++r->sequence; r->count = r->flags = 0;
}
static void codec_task(void *arg)
{
    struct CODEC2 *codec = arg;
    uint32_t epoch = UINT32_MAX;
    voice_record_t pending = {0};
    voice_playout_t playout;
    voice_playout_reset(&playout);
    for (;;) {
        const session_t s = session_get();
        if (s.epoch != epoch) {
            epoch = s.epoch;
            codec2_destroy(codec); codec = codec2_create(CODEC2_MODE_700C);
            pending = (voice_record_t){.flags = VOICE_FLAG_START};
            voice_playout_reset(&playout);
            portENTER_CRITICAL(&s_lock); s_status.codec_ready = codec != NULL; portEXIT_CRITICAL(&s_lock);
            if (!codec) ESP_LOGE("voice", "Codec2 allocation failed");
        }
        if (codec && s.type == CONTENT_AUDIO && s.transmit) {
            pcm_item_t in;
            if (xQueueReceive(s_input, &in, 0) == pdTRUE && in.epoch == epoch) {
                const int64_t start = esp_timer_get_time();
                if (in.valid) {
                    codec2_encode(codec, pending.frames[pending.count++], in.pcm);
                    portENTER_CRITICAL(&s_lock);
                    if (s_status.epoch == epoch) {
                        ++s_status.encoded_frames;
                        memcpy(s_status.last_voice_frame, pending.frames[pending.count - 1], VOICE_FRAME_BYTES);
                    }
                    portEXIT_CRITICAL(&s_lock);
                }
                if (in.end) pending.flags |= VOICE_FLAG_END;
                if (pending.count == 2 || in.end) enqueue_record(&pending, epoch);
                portENTER_CRITICAL(&s_lock);
                if (s_status.epoch == epoch) {
                    const uint32_t elapsed = esp_timer_get_time() - start;
                    if (elapsed > s_status.codec_max_us) s_status.codec_max_us = elapsed;
                    if (in.end) s_encoder_ended = true;
                }
                portEXIT_CRITICAL(&s_lock);
            }
        } else if (codec && s.type == CONTENT_AUDIO) {
            const uint32_t now = esp_timer_get_time() / 1000;
            record_item_t item;
            const uint32_t dropped_before = playout.dropped_frames;
            for (unsigned i = 0; i < 8 && xQueueReceive(s_records, &item, 0) == pdTRUE; ++i)
                if (item.epoch == epoch) (void)voice_playout_push(&playout, &item.record, now);
            portENTER_CRITICAL(&s_lock);
            if (s_status.epoch == epoch) {
                s_status.lost_packets = playout.lost_packets; s_status.duplicates = playout.duplicates;
                s_status.queue_drops += playout.dropped_frames - dropped_before;
            }
            portEXIT_CRITICAL(&s_lock);
            uint8_t bits[VOICE_FRAME_BYTES];
            const int kind = voice_playout_next(&playout, now, bits);
            if (kind) {
                pcm_item_t out = {.epoch = epoch};
                out.end = playout.ended && playout.count == 0;
                const int64_t start = esp_timer_get_time();
                if (kind == 1) codec2_decode(codec, out.pcm, bits);
                /* Erasures produce bounded silence, never decode corrupt bits. */
                if (session_get().epoch == epoch && xQueueSend(s_output, &out, 0) != pdTRUE) count_drop();
                portENTER_CRITICAL(&s_lock);
                if (s_status.epoch == epoch) {
                    if (kind == 1) ++s_status.decoded_frames; else ++s_status.concealed_frames;
                    const uint32_t elapsed = esp_timer_get_time() - start;
                    if (elapsed > s_status.codec_max_us) s_status.codec_max_us = elapsed;
                    s_status.lost_packets = playout.lost_packets; s_status.duplicates = playout.duplicates;
                }
                portEXIT_CRITICAL(&s_lock);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}
bool content_play_audio(void)
{
    static voice_upsample_t up;
    static uint32_t epoch = UINT32_MAX;
    static int16_t pcm48k[VOICE_FRAME_SAMPLES * 6];
    const session_t s = session_get();
    if (s.epoch != epoch) { memset(&up, 0, sizeof(up)); epoch = s.epoch; }
    if (s.type != CONTENT_AUDIO || s.transmit) return false;
    pcm_item_t item;
    if (xQueueReceive(s_output, &item, 0) != pdTRUE || item.epoch != epoch) return false;
    const size_t n = voice_upsample(&up, item.pcm, VOICE_FRAME_SAMPLES, pcm48k);
    if (audio_output_write(pcm48k, n) != n) {
        portENTER_CRITICAL(&s_lock); ++s_status.output_errors; portEXIT_CRITICAL(&s_lock);
    }
    if (item.end) {
        const int16_t silence[VOICE_RESAMPLE_TAPS / 6] = {0};
        const size_t tail = voice_upsample(&up, silence, VOICE_RESAMPLE_TAPS / 6, pcm48k);
        (void)audio_output_write(pcm48k, tail);
    }
    return true;
}
esp_err_t content_service_init(void)
{
    s_packets = xQueueCreate(32, sizeof(packet_item_t)); /* covers initial modem training */
    s_input = xQueueCreate(8, sizeof(pcm_item_t));
    s_records = xQueueCreate(8, sizeof(record_item_t));
    s_output = xQueueCreate(4, sizeof(pcm_item_t));
    if (!s_packets || !s_input || !s_records || !s_output) return ESP_ERR_NO_MEM;
    struct CODEC2 *codec = codec2_create(CODEC2_MODE_700C);
    if (!codec) return ESP_ERR_NO_MEM;
    if (codec2_samples_per_frame(codec) != VOICE_FRAME_SAMPLES || codec2_bits_per_frame(codec) != VOICE_FRAME_BITS) {
        codec2_destroy(codec); return ESP_ERR_INVALID_SIZE;
    }
    s_status.codec_ready = true;
    if (xTaskCreatePinnedToCore(codec_task, "codec2", 24576, codec, 6, NULL, 1) != pdPASS) {
        codec2_destroy(codec); return ESP_ERR_NO_MEM;
    }
    ESP_LOGI("voice", "Codec2 700C ready: 8 kHz, 320 samples / 28 bits; no UART module");
    return ESP_OK;
}
