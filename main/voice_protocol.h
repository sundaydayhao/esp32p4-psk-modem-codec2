#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Project wire protocol v1, Codec2 700C. This is NOT a FreeDV air frame. */
#define VOICE_SYNC_WORD 0xC270C2A5U
#define VOICE_RATE 700U
#define VOICE_SAMPLE_RATE 8000U
#define VOICE_FRAME_SAMPLES 320U
#define VOICE_FRAME_BITS 28U
#define VOICE_FRAME_BYTES 4U
#define VOICE_PACKET_MAX 15U
#define VOICE_FLAG_START 0x08U
#define VOICE_FLAG_END 0x04U

typedef struct { uint8_t data[VOICE_PACKET_MAX]; size_t len; } voice_packet_t;
typedef struct {
    uint8_t frames[2][VOICE_FRAME_BYTES];
    uint8_t sequence, count, flags;
} voice_record_t;
typedef void (*voice_record_cb_t)(const voice_record_t *record, void *user);
typedef struct {
    uint8_t data[VOICE_PACKET_MAX];
    size_t used;
    uint32_t packets, frames, crc_errors, bad_descriptors, sync_drops;
} voice_wire_rx_t;

size_t voice_encode(const voice_record_t *record, voice_packet_t *packet);
void voice_wire_feed(voice_wire_rx_t *rx, const uint8_t *data, size_t len,
                     voice_record_cb_t callback, void *user);
