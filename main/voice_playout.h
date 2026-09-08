#pragma once
#include "voice_protocol.h"
#define VOICE_PLAYOUT_CAPACITY 16U
typedef struct {
    uint8_t data[VOICE_PLAYOUT_CAPACITY][VOICE_FRAME_BYTES];
    uint8_t erased[VOICE_PLAYOUT_CAPACITY];
    unsigned head, count, conceal_debt;
    uint8_t next_sequence, have_sequence, ended, playing, underflows;
    uint32_t due_ms, last_rx_ms, lost_packets, duplicates, dropped_frames;
} voice_playout_t;
void voice_playout_reset(voice_playout_t *p);
bool voice_playout_push(voice_playout_t *p, const voice_record_t *r, uint32_t now_ms);
/* One 40 ms frame: 0 idle, 1 coded frame, 2 silence for a missing frame. */
int voice_playout_next(voice_playout_t *p, uint32_t now_ms, uint8_t bits[VOICE_FRAME_BYTES]);
