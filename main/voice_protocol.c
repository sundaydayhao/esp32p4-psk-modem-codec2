#include "voice_protocol.h"
#include <string.h>

static const uint8_t sync_word[] = {0xc2, 0x70, 0xc2, 0xa5};
static uint16_t crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xffff;
    while (len--) {
        crc ^= (uint16_t)*data++ << 8;
        for (unsigned b = 0; b < 8; ++b)
            crc = (uint16_t)((crc << 1) ^ ((crc & 0x8000) ? 0x1021 : 0));
    }
    return crc;
}

size_t voice_encode(const voice_record_t *r, voice_packet_t *p)
{
    if (!r || !p || r->count > 2 || (r->flags & ~(VOICE_FLAG_START | VOICE_FLAG_END)) ||
        (r->count < 2 && !(r->flags & VOICE_FLAG_END))) return 0;
    memset(p, 0, sizeof(*p));
    memcpy(p->data, sync_word, 4);
    /* version=1 (bits 7:6), codec=700C (bits 5:4), flags, frame count. */
    p->data[4] = 0x50 | r->flags | r->count;
    p->data[5] = r->sequence;
    for (unsigned f = 0; f < r->count; ++f)
        for (unsigned b = 0; b < VOICE_FRAME_BITS; ++b) {
            const unsigned dst = f * VOICE_FRAME_BITS + b;
            if (r->frames[f][b / 8] & (0x80U >> (b % 8)))
                p->data[6 + dst / 8] |= 0x80U >> (dst % 8);
        }
    const uint16_t crc = crc16(p->data + 4, 9);
    p->data[13] = (uint8_t)(crc >> 8); p->data[14] = (uint8_t)crc;
    p->len = VOICE_PACKET_MAX;
    return p->len;
}

void voice_wire_feed(voice_wire_rx_t *rx, const uint8_t *data, size_t len,
                     voice_record_cb_t callback, void *user)
{
    if (!rx || (!data && len)) return;
    for (size_t i = 0; i < len; ++i) {
        rx->data[rx->used++] = data[i];
        while (rx->used && memcmp(rx->data, sync_word, rx->used < 4 ? rx->used : 4)) {
            if (rx->data[0]) ++rx->sync_drops; /* binary zero is idle */
            memmove(rx->data, rx->data + 1, --rx->used);
        }
        if (rx->used < VOICE_PACKET_MAX) continue;
        const uint8_t desc = rx->data[4];
        const unsigned count = desc & 3;
        const uint16_t received = ((uint16_t)rx->data[13] << 8) | rx->data[14];
        bool valid = false;
        if ((desc & 0xf0) != 0x50 || count > 2 ||
            (count < 2 && !(desc & VOICE_FLAG_END))) ++rx->bad_descriptors;
        else if (received != crc16(rx->data + 4, 9)) ++rx->crc_errors;
        else {
            voice_record_t r = {.count = count, .sequence = rx->data[5],
                .flags = desc & (VOICE_FLAG_START | VOICE_FLAG_END)};
            for (unsigned f = 0; f < count; ++f)
                for (unsigned b = 0; b < VOICE_FRAME_BITS; ++b) {
                    const unsigned src = f * VOICE_FRAME_BITS + b;
                    if (rx->data[6 + src / 8] & (0x80U >> (src % 8)))
                        r.frames[f][b / 8] |= 0x80U >> (b % 8);
                }
            ++rx->packets; rx->frames += count;
            if (callback) callback(&r, user);
            valid = true;
        }
        /* A failed frame may contain the next sync after a byte insertion/loss. */
        if (valid) rx->used = 0;
        else memmove(rx->data, rx->data + 1, --rx->used);
    }
}
