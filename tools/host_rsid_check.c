#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "rsid.h"
static void check(psk_mode_t mode, int offset, int erase)
{
    static rsid_rx_t rx;
    rsid_rx_reset(&rx, 1500); rsid_tx_t tx;
    assert(rsid_tx_start(&tx, mode, 1500 + offset, 48000));
    unsigned detections = 0; rsid_match_t match = {0};
    for (unsigned pos = 0; pos < 48000 * 3; pos += 251) {
        int16_t pcm[251] = {0};
        rsid_tx_generate(&tx, pcm, 251);
        if (erase && pos / tx.symbol_samples == 11) memset(pcm, 0, sizeof(pcm));
        if (rsid_rx_feed(&rx, pcm, 251, &match)) {
            ++detections;
            assert(match.mode == mode);
            assert(fabsf(match.tone_hz - (1500 + offset)) < 4);
        }
    }
    printf("RsID %s offset=%d erase=%d detections=%u tone=%.2f errors=%u\n",
        psk_mode_name(mode), offset, erase, detections, match.tone_hz, match.errors);
    assert(detections == 1);
}
int main(void)
{
    uint8_t tones[15];
    /* Fixed primary-RsID vectors independently evaluated from fldigi's GF table. */
    const uint8_t p125r[15] = {0,14,11,1,12,15,9,5,4,6,2,10,3,8,7};
    const uint8_t p250r[15] = {0,14,7,14,13,0,4,9,7,4,3,9,13,10,10};
    rsid_encode(183, tones); assert(!memcmp(tones, p125r, 15));
    rsid_encode(186, tones); assert(!memcmp(tones, p250r, 15));
    for (unsigned m = 0; m < PSK_MODE_COUNT; ++m) {
        uint16_t id = rsid_mode_id(m); if (!id) continue;
        rsid_encode(id, tones); printf("CODE %u",id);
        for (unsigned t = 0; t < 15; ++t) printf(" %u",tones[t]);
        puts("");
    }
    check(PSK_MODE_PSK125R,0,0); check(PSK_MODE_PSK250R,20,0);
    check(PSK_MODE_PSK125R,-20,1); check(PSK_MODE_BPSK31,0,0);
    rsid_tx_t tx; assert(!rsid_tx_start(&tx, PSK_MODE_8PSK1200F,1500,48000));
    static rsid_rx_t rx; rsid_rx_reset(&rx,1500);
    uint32_t rng = 723456; rsid_match_t match;
    for (unsigned n = 0; n < 48000 * 10; n += 256) {
        int16_t noise[256];
        for (unsigned j = 0; j < 256; ++j) { rng = 1664525 * rng + 1013904223; noise[j] = (int16_t)(rng >> 16); }
        assert(!rsid_rx_feed(&rx,noise,256,&match));
    }
    puts("PASS RsID audio identification, frequency estimate, 1-symbol erasure, noise rejection");
}
