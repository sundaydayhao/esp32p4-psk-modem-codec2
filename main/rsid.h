#pragma once
#include <stdbool.h>
#include "psk_modes.h"
#include "voice_resample.h"
#define RSID_SYMBOLS 15U
#define RSID_BINS 85U
#define RSID_CENTERS 25U
typedef struct {
    uint8_t tones[15];
    uint32_t sample_rate, symbol_samples, position;
    float phase, center;
    bool active;
} rsid_tx_t;
typedef struct { psk_mode_t mode; uint16_t id; float tone_hz; unsigned errors; } rsid_match_t;
typedef struct {
    voice_downsample_t down;
    float window[744], coefficients[RSID_BINS];
    uint8_t history[RSID_CENTERS][60];
    float confidence[RSID_CENTERS][60];
    unsigned pos, hop, history_pos, windows, cooldown;
    float center;
} rsid_rx_t;
uint16_t rsid_mode_id(psk_mode_t mode);
void rsid_encode(uint16_t id, uint8_t tones[15]);
bool rsid_tx_start(rsid_tx_t *tx, psk_mode_t mode, float center, uint32_t rate);
size_t rsid_tx_generate(rsid_tx_t *tx, int16_t *pcm, size_t capacity);
/* Supported primary IDs only; normal USB, search +/-32 Hz around selected tone.
 * No arbitrary modulation detection and no extended-ID support. */
void rsid_rx_reset(rsid_rx_t *rx, float center);
bool rsid_rx_feed(rsid_rx_t *rx, const int16_t *pcm48k, size_t count, rsid_match_t *match);
