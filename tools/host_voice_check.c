#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "codec2.h"
#include "voice_protocol.h"
#include "voice_playout.h"
#include "voice_resample.h"
#include "psk_tx.h"
#include "psk_rx.h"
#include "audio_decim.h"

static voice_record_t records[90], received[96];
static voice_packet_t packets[90];
static unsigned n_received;
static void receive(const voice_record_t *r, void *user)
{
    (void)user; assert(n_received < 96); received[n_received++] = *r;
}
static void protocol_test(void)
{
    const voice_record_t golden = {.frames = {{0x12,0x34,0x56,0x70},{0x89,0xab,0xcd,0xe0}},
        .count = 2, .flags = VOICE_FLAG_START};
    const uint8_t expected_wire[15] = {0xc2,0x70,0xc2,0xa5,0x5a,0,0x12,0x34,0x56,0x78,0x9a,0xbc,0xde,0x54,0x52};
    voice_packet_t golden_packet;
    assert(voice_encode(&golden, &golden_packet) == 15 && !memcmp(golden_packet.data, expected_wire, 15));
    for (unsigned n = 0; n < 90; ++n) {
        records[n] = (voice_record_t){.sequence = (uint8_t)n, .count = 2};
        if (!n) records[n].flags = VOICE_FLAG_START;
        if (n == 89) { records[n].flags = VOICE_FLAG_END; records[n].count = 1; }
        for (unsigned f = 0; f < records[n].count; ++f)
            for (unsigned b = 0; b < 4; ++b) records[n].frames[f][b] = (uint8_t)(n * 73 + f * 51 + b * 47);
        for (unsigned f = 0; f < 2; ++f) records[n].frames[f][3] &= 0xf0;
        assert(voice_encode(&records[n], &packets[n]) == 15);
    }
    voice_wire_rx_t rx = {0}; n_received = 0;
    for (unsigned n = 0; n < 90; ++n)
        for (unsigned b = 0; b < 15; ++b) voice_wire_feed(&rx, packets[n].data + b, 1, receive, NULL);
    assert(n_received == 90 && !memcmp(records, received, sizeof(records)));
    /* All single-bit corruptions, arbitrary fragments, inserted/lost bytes. */
    for (unsigned bit = 0; bit < 120; ++bit) {
        voice_packet_t bad = packets[0]; bad.data[bit / 8] ^= 1 << (bit % 8);
        rx = (voice_wire_rx_t){0}; n_received = 0;
        voice_wire_feed(&rx, bad.data, 15, receive, NULL);
        voice_wire_feed(&rx, packets[1].data, 15, receive, NULL);
        assert(n_received == 1 && received[0].sequence == 1);
    }
    for (unsigned at = 0; at < 15; ++at) {
        rx = (voice_wire_rx_t){0}; n_received = 0;
        voice_wire_feed(&rx, packets[0].data, at, receive, NULL);
        voice_wire_feed(&rx, packets[0].data + at + 1, 14 - at, receive, NULL);
        voice_wire_feed(&rx, packets[1].data, 15, receive, NULL);
        assert(n_received == 1 && received[0].sequence == 1);
        rx = (voice_wire_rx_t){0}; n_received = 0; const uint8_t junk = 0x35;
        voice_wire_feed(&rx, packets[0].data, at, receive, NULL);
        voice_wire_feed(&rx, &junk, 1, receive, NULL);
        voice_wire_feed(&rx, packets[0].data + at, 15 - at, receive, NULL);
        voice_wire_feed(&rx, packets[1].data, 15, receive, NULL);
        assert(received[n_received - 1].sequence == 1);
    }
    puts("PASS Codec2 wire: 28-bit packing, CRC, all bit flips, byte loss/insertion, fragmentation");
}
static void playout_test(void)
{
    voice_playout_t p; voice_playout_reset(&p); uint8_t bits[4];
    assert(voice_playout_push(&p, &records[0], 0));
    assert(!voice_playout_push(&p, &records[0], 1));
    assert(voice_playout_next(&p, 79, bits) == 0);
    assert(voice_playout_next(&p, 80, bits) == 1);
    assert(voice_playout_next(&p, 119, bits) == 0);
    assert(voice_playout_next(&p, 120, bits) == 1);
    assert(voice_playout_push(&p, &records[2], 121));
    assert(p.lost_packets == 1);
    assert(voice_playout_next(&p, 160, bits) == 2);
    assert(voice_playout_next(&p, 200, bits) == 2);
    assert(voice_playout_next(&p, 240, bits) == 1);
    assert(voice_playout_next(&p, 280, bits) == 1);
    voice_record_t end = {.sequence = 3, .flags = VOICE_FLAG_END};
    assert(voice_playout_push(&p, &end, 281));
    assert(voice_playout_next(&p, 320, bits) == 0);
    assert(voice_playout_next(&p, 3000, bits) == 0);
    voice_playout_reset(&p);
    voice_record_t r = records[0]; r.sequence = 255;
    voice_playout_push(&p, &r, UINT32_MAX - 100);
    r.flags = 0; r.sequence = 0;
    assert(voice_playout_push(&p, &r, UINT32_MAX - 20));
    assert(voice_playout_next(&p, UINT32_MAX - 20, bits) == 1);
    assert(voice_playout_next(&p, 19, bits) == 1);
    for (unsigned t = 59; t < 500; t += 40) voice_playout_next(&p, t, bits);
    assert(!p.playing && p.underflows == 3);
    /* Late packets must not replay time already concealed as silence. */
    voice_playout_reset(&p); voice_playout_push(&p, &records[0], 0);
    voice_playout_next(&p, 80, bits); voice_playout_next(&p, 120, bits);
    assert(voice_playout_next(&p, 160, bits) == 2);
    assert(voice_playout_next(&p, 200, bits) == 2);
    voice_playout_push(&p, &records[2], 205);
    assert(voice_playout_next(&p, 240, bits) == 1);
    assert(!memcmp(bits, records[2].frames[0], 4));
    /* A new talkspurt can start even when the previous END was lost. */
    assert(voice_playout_push(&p, &records[0], 250));
    assert(p.count == 2 && p.next_sequence == 1);
    puts("PASS playout: 40 ms clock, 80 ms startup, duplicates, sequence/time wrap, loss, END and lost-END timeout");
}
static void codec_test(void)
{
    struct CODEC2 *enc = codec2_create(CODEC2_MODE_700C), *dec = codec2_create(CODEC2_MODE_700C);
    assert(enc && dec && codec2_bits_per_frame(enc) == 28 && codec2_samples_per_frame(enc) == 320);
    long long energy = 0;
    for (unsigned f = 0; f < 24; ++f) {
        short speech[320], out[320]; uint8_t bits[4];
        for (unsigned i = 0; i < 320; ++i)
            speech[i] = (short)(6000 * sin(6.2831853 * 180 * (i + 320 * f) / 8000));
        codec2_encode(enc, bits, speech); codec2_decode(dec, out, bits);
        for (unsigned i = 0; i < 320; ++i) energy += (long long)out[i] * out[i];
    }
    assert(energy > 100000); codec2_destroy(enc); codec2_destroy(dec);
    voice_downsample_t down = {0}; voice_upsample_t up = {0};
    int16_t in[256] = {0}, speech[43], out[258]; size_t total = 0;
    for (unsigned n = 0; n < 30; ++n) {
        size_t used = voice_downsample(&down, in, 256, speech); total += used;
        assert(voice_upsample(&up, speech, used, out) == used * 6);
    }
    assert(total == 1280);
    puts("PASS actual upstream Codec2 700C encode/decode, streaming 48/8 kHz sample counts");
}
static size_t fetch_pos;
static unsigned sample_clock;
static int fetch(void *user)
{
    (void)user;
    if (fetch_pos == 90 * 15) return -2;
    if (sample_clock < (fetch_pos / 15) * 3840) return -1;
    const int out = packets[fetch_pos / 15].data[fetch_pos % 15]; ++fetch_pos; return out;
}
static void audio_link(psk_mode_t mode)
{
    static psk_tx_t tx; static psk_rx_t rx; voice_wire_rx_t wire = {0}; audio_decim_state_t filter;
    audio_decim_init(&filter); psk_tx_init(&tx, 48000);
    psk_rx_config_t cfg = {.mode = mode, .sample_rate_hz = 48000, .rx_tone_hz = 2000,
        .squelch = 20, .afc_enabled = 1, .squelch_enabled = 0};
    psk_rx_init(&rx, &cfg); psk_rx_set_binary_sync(&rx, VOICE_SYNC_WORD);
    fetch_pos = 0; sample_clock = 0; n_received = 0;
    assert(psk_tx_start_binary_stream(&tx, mode, 2000, fetch, NULL) == 0);
    for (; sample_clock < 48000 * 15; sample_clock += 256) {
        int16_t pcm[256] = {0}, filtered[256]; char bytes[256];
        psk_tx_generate(&tx, pcm, 256);
        size_t n = audio_decim_process(&filter, pcm, 256, filtered, 1);
        n = psk_rx_process(&rx, filtered, n, bytes, sizeof(bytes));
        voice_wire_feed(&wire, (uint8_t *)bytes, n, receive, NULL);
    }
    printf("%s voice: received=%u/90 CRC=%u, tail=%u active=%d\n", psk_mode_name(mode), n_received,
        wire.crc_errors, n_received ? received[n_received-1].flags : 0, psk_tx_active(&tx));
    assert(!psk_tx_active(&tx) && n_received == 90 && !memcmp(received, records, sizeof(records)));
}
int main(void)
{
    protocol_test(); playout_test(); codec_test();
    audio_link(PSK_MODE_8PSK1200F); audio_link(PSK_MODE_8PSK1000F);
    puts("PASS continuous 80 ms Codec2 packets over both single-carrier FEC audio modems, complete final frame");
    return 0;
}
