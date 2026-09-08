#include "voice_playout.h"
#include <string.h>

void voice_playout_reset(voice_playout_t *p) { memset(p, 0, sizeof(*p)); }
static void append(voice_playout_t *p, const uint8_t *bits)
{
    if (p->count == VOICE_PLAYOUT_CAPACITY) {
        p->head = (p->head + 1) % VOICE_PLAYOUT_CAPACITY;
        --p->count; ++p->dropped_frames;
    }
    const unsigned pos = (p->head + p->count++) % VOICE_PLAYOUT_CAPACITY;
    p->erased[pos] = bits == NULL;
    if (bits) memcpy(p->data[pos], bits, VOICE_FRAME_BYTES);
}
bool voice_playout_push(voice_playout_t *p, const voice_record_t *r, uint32_t now)
{
    const bool restart = !p->have_sequence || (uint32_t)(now - p->last_rx_ms) > 1000 ||
        ((r->flags & VOICE_FLAG_START) && (p->ended || !p->playing ||
            p->next_sequence != (uint8_t)(r->sequence + 1)));
    if (restart) {
        p->count = p->head = p->playing = p->ended = p->underflows = 0;
        p->conceal_debt = 0;
        p->have_sequence = 1; p->next_sequence = r->sequence;
    }
    const uint8_t gap = r->sequence - p->next_sequence;
    if (gap >= 128) { ++p->duplicates; return false; }
    p->lost_packets += gap;
    if (gap > 4) { p->count = p->head = p->playing = p->conceal_debt = 0; }
    else for (unsigned i = 0; i < 2U * gap; ++i) {
        if (p->conceal_debt) --p->conceal_debt; else append(p, NULL);
    }
    p->next_sequence = r->sequence + 1; p->last_rx_ms = now;
    for (unsigned i = 0; i < r->count; ++i) {
        if (p->conceal_debt) { --p->conceal_debt; ++p->dropped_frames; }
        else append(p, r->frames[i]);
    }
    p->ended = (r->flags & VOICE_FLAG_END) != 0;
    if (!p->playing && p->count) {
        p->playing = 1; p->due_ms = now + 80; p->underflows = 0;
    }
    return true;
}
int voice_playout_next(voice_playout_t *p, uint32_t now, uint8_t bits[VOICE_FRAME_BYTES])
{
    if (!p->playing || (int32_t)(now - p->due_ms) < 0) return 0;
    /* Do not burst a backlog after a stalled consumer. */
    p->due_ms = (uint32_t)(now - p->due_ms) > 40 ? now + 40 : p->due_ms + 40;
    if (!p->count) {
        if (p->ended || p->underflows >= 3) { p->playing = 0; return 0; }
        ++p->underflows; ++p->conceal_debt; memset(bits, 0, VOICE_FRAME_BYTES); return 2;
    }
    p->underflows = 0;
    const unsigned pos = p->head;
    p->head = (p->head + 1) % VOICE_PLAYOUT_CAPACITY; --p->count;
    memcpy(bits, p->data[pos], VOICE_FRAME_BYTES);
    return p->erased[pos] ? 2 : 1;
}
