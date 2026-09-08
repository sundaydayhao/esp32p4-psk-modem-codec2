/* Image protocol and single-carrier audio regression. */
#include "modem_image.h"
#include "psk_rx.h"
#include "psk_tx.h"
#include "psk_codec.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static uint8_t pixels[9216], stream[65536];
static modem_image_rx_parser_t image_rx;
static modem_image_tx_stream_t image_tx;
static psk_tx_t tx;
static psk_rx_t rx;
static void row_cb(uint16_t row, const uint8_t *data, size_t len, uint16_t complete, void *user)
{
    (void)complete; (void)user;
    assert(row < 96 && len == 96);
    memcpy(pixels + row * 96, data, len);
}
static void image_protocol(void)
{
    uint8_t zero[96] = {0};
    assert(modem_image_row_crc(0, zero, 96) == 0x1cc2);
    modem_image_tx_stream_reset(&image_tx);
    for (size_t i = 0; i < 19514; ++i) stream[i] = modem_image_tx_stream_fetch(&image_tx);
    assert(!memcmp(stream, "~IMG:96:96:RGB332\n", 18));
    uint32_t psk_bits = 0, mfsk_bits = 0;
    for (size_t i = 0; i < 19514 - MODEM_IMAGE_GAP_CHARS; ++i) {
        uint32_t code = psk_codec_varicode_code(stream[i]);
        while (code) { ++psk_bits; code >>= 1; }
        psk_bits += 2;
        code = psk_codec_mfsk_varicode_table()[stream[i]];
        while (code) { ++mfsk_bits; code >>= 1; }
    }
    printf("Image encoded bits excluding repeat gap: PSK=%lu MFSK=%lu\n", (unsigned long)psk_bits, (unsigned long)mfsk_bits);
    assert(psk_bits == MODEM_IMAGE_PSK_BITS && mfsk_bits == MODEM_IMAGE_MFSK_BITS);
    modem_image_rx_parser_init(&image_rx);
    /* Lose the header and corrupt row 17; all other rows must survive. */
    stream[18 + 17 * 203 + 5] ^= 1;
    for (size_t i = 18; i < 19514; ++i)
        modem_image_rx_parser_feed(&image_rx, (const char *)stream + i, 1, NULL, row_cb, NULL);
    assert(image_rx.valid_rows == 95 && image_rx.total_invalid_rows == 1 && image_rx.completed_frames == 0);
    stream[18 + 17 * 203 + 5] ^= 1;
    /* A repeated header retains incomplete rows; a valid repeat repairs the hole. */
    modem_image_rx_parser_feed(&image_rx, (const char *)stream, 18, NULL, row_cb, NULL);
    assert(image_rx.valid_rows == 95);
    modem_image_rx_parser_feed(&image_rx, (const char *)stream + 18 + 17 * 203, 203, NULL, row_cb, NULL);
    assert(image_rx.valid_rows == 96 && image_rx.completed_frames == 1);
    assert(!memcmp(pixels, g_modem_test_image_rgb332, sizeof(pixels)));
    puts("PASS image: HPM stream length, header loss, row CRC, repeat repair, all 9216 pixels");
}
static void setup(int unused) {
    (void)unused; psk_tx_init(&tx, 48000);
    psk_rx_config_t cfg = {.mode=PSK_MODE_8PSK1200F,.sample_rate_hz=48000,
        .rx_tone_hz=2000,.squelch=20,.afc_enabled=1,.squelch_enabled=0};
    psk_rx_init(&rx,&cfg);
}
static void image_roundtrip(void)
{
    setup(0); memset(pixels, 0, sizeof(pixels));
    modem_image_rx_parser_init(&image_rx); modem_image_tx_stream_reset(&image_tx);
    assert(!psk_tx_start_stream(&tx, PSK_MODE_8PSK1200F, 2000, modem_image_tx_stream_fetch, &image_tx));
    uint32_t samples = 0;
    for (; samples < 48000 * 180 && !image_rx.completed_frames; samples += 256) {
        int16_t pcm[256]; char out[256];
        assert(psk_tx_generate(&tx, pcm, 256) == 256);
        const size_t n = psk_rx_process(&rx, pcm, 256, out, sizeof(out));
        modem_image_rx_parser_feed(&image_rx, out, n, NULL, row_cb, NULL);
    }
    assert(image_rx.completed_frames && !memcmp(pixels, g_modem_test_image_rgb332, sizeof(pixels)));
    printf("PASS PSK image: all 96 CRC rows / 9216 pixels exact, bad rows %lu, %.3f audio seconds\n",
        (unsigned long)image_rx.total_invalid_rows, samples / 48000.0);
}
int main(void) { image_protocol(); image_roundtrip(); return 0; }
