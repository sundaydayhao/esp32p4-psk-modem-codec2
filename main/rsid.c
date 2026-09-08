#include "rsid.h"
#include <math.h>
#include <string.h>

#define TWO_PI 6.2831853071795864769f
#define SPACING (11025.0f / 1024.0f)
/* Primary assignments published by fldigi rsid_defs.cxx. Codec2's custom
 * binary stream deliberately has no standard text-mode or FreeDV ID. */
static const struct { psk_mode_t mode; uint16_t id; } assignments[] = {
    {PSK_MODE_BPSK31, 1}, {PSK_MODE_BPSK63, 2}, {PSK_MODE_BPSK125, 4},
    {PSK_MODE_BPSK250, 126}, {PSK_MODE_BPSK500, 173},
    {PSK_MODE_QPSK31, 110}, {PSK_MODE_QPSK63, 3},
    {PSK_MODE_QPSK125, 5}, {PSK_MODE_QPSK250, 127},
    {PSK_MODE_PSK125R, 183}, {PSK_MODE_PSK250R, 186},
};
uint16_t rsid_mode_id(psk_mode_t mode)
{
    for (unsigned i = 0; i < sizeof(assignments)/sizeof(assignments[0]); ++i)
        if (assignments[i].mode == mode) return assignments[i].id;
    return 0;
}
static uint8_t multiply(uint8_t a, uint8_t b)
{
    uint8_t p = 0;
    while (b) {
        if (b & 1) p ^= a;
        b >>= 1; a <<= 1;
        if (a & 16) a ^= 0x19; /* GF(16), x^4+x^3+1 */
    }
    return p;
}
void rsid_encode(uint16_t id, uint8_t tones[15])
{
    static const uint8_t roots[] = {2,4,8,9,11,15,7,14,5,10,13,3};
    memset(tones, 0, 15); tones[0] = (id >> 8) & 15; tones[1] = (id >> 4) & 15; tones[2] = id & 15;
    for (unsigned i = 0; i < 12; ++i) {
        for (int j = 14; j > 0; --j) tones[j] = tones[j-1] ^ multiply(tones[j], roots[i]);
        tones[0] = multiply(tones[0], roots[i]);
    }
}
bool rsid_tx_start(rsid_tx_t *tx, psk_mode_t mode, float center, uint32_t rate)
{
    memset(tx, 0, sizeof(*tx));
    const uint16_t id = rsid_mode_id(mode);
    if (!id || rate < 8000 || center < 200 || center > 3400) return false;
    rsid_encode(id, tx->tones); tx->center = center; tx->sample_rate = rate;
    tx->symbol_samples = (uint32_t)(1024.0 * rate / 11025.0); tx->active = true;
    return true;
}
size_t rsid_tx_generate(rsid_tx_t *tx, int16_t *pcm, size_t capacity)
{
    size_t n = 0;
    while (tx->active && n < capacity) {
        const unsigned symbol = tx->position / tx->symbol_samples;
        if (symbol >= 25) { tx->active = false; break; }
        float value = 0;
        if (symbol >= 5 && symbol < 20) {
            const float freq = tx->center + ((float)tx->tones[symbol - 5] - 7) * SPACING;
            tx->phase += TWO_PI * freq / tx->sample_rate;
            if (tx->phase > TWO_PI) tx->phase -= TWO_PI;
            value = 18000 * sinf(tx->phase);
        }
        pcm[n++] = (int16_t)value; ++tx->position;
    }
    return n;
}
void rsid_rx_reset(rsid_rx_t *rx, float center)
{
    memset(rx, 0, sizeof(*rx)); rx->center = center;
    memset(rx->history, 255, sizeof(rx->history));
    for (unsigned i = 0; i < RSID_BINS; ++i) {
        const float f = center + ((float)i / 4 - 10) * SPACING;
        rx->coefficients[i] = 2 * cosf(TWO_PI * f / 8000);
    }
}
static bool analyse(rsid_rx_t *rx, rsid_match_t *match)
{
    float power[RSID_BINS];
    for (unsigned bin = 0; bin < RSID_BINS; ++bin) {
        float a = 0, b = 0;
        for (unsigned j = 0; j < 744; ++j) {
            const float x = rx->window[(rx->pos + j) % 744];
            const float c = x + rx->coefficients[bin] * a - b;
            b = a; a = c;
        }
        power[bin] = a*a + b*b - rx->coefficients[bin]*a*b;
    }
    const unsigned pos = rx->history_pos;
    for (unsigned c = 0; c < RSID_CENTERS; ++c) {
        float sum = 0, best = 0; uint8_t tone = 255;
        for (unsigned t = 0; t < 16; ++t) {
            const float p = power[c + 4 * t]; sum += p;
            if (p > best) { best = p; tone = t; }
        }
        rx->history[c][pos] = best > 1e8f && best > 0.4f * sum ? tone : 255;
        rx->confidence[c][pos] = best;
    }
    rx->history_pos = (pos + 1) % 60;
    if (++rx->windows < 60) return false;
    if (rx->cooldown) { --rx->cooldown; return false; }
    float best_score = 0; unsigned best_errors = 2; bool found = false;
    for (unsigned a = 0; a < sizeof(assignments)/sizeof(assignments[0]); ++a) {
        uint8_t tones[15]; rsid_encode(assignments[a].id, tones);
        for (unsigned c = 0; c < RSID_CENTERS; ++c) {
            unsigned errors = 0; float score = 0;
            for (unsigned i = 0; i < 15; ++i) {
                const unsigned h = (pos + 60 - 4 * (14 - i)) % 60;
                errors += rx->history[c][h] != tones[i];
                score += rx->confidence[c][h];
            }
            if (errors < 2 && (errors < best_errors || (errors == best_errors && score > best_score))) {
                best_errors = errors; best_score = score; found = true;
                *match = (rsid_match_t){assignments[a].mode, assignments[a].id,
                    rx->center + ((float)c / 4 - 3) * SPACING, errors};
            }
        }
    }
    if (found) rx->cooldown = 100; /* 2.325 s duplicate suppression */
    return found;
}
bool rsid_rx_feed(rsid_rx_t *rx, const int16_t *pcm, size_t count, rsid_match_t *match)
{
    bool found = false;
    while (count) {
        const size_t n = count > 256 ? 256 : count;
        int16_t down[43]; const size_t used = voice_downsample(&rx->down, pcm, n, down);
        pcm += n; count -= n;
        for (size_t i = 0; i < used; ++i) {
            rx->window[rx->pos] = down[i]; rx->pos = (rx->pos + 1) % 744;
            if (++rx->hop == 186) { rx->hop = 0; if (analyse(rx, match)) found = true; }
        }
    }
    return found;
}
