#include "voice_resample.h"
static const float coefficients[VOICE_RESAMPLE_TAPS] = {
#include "voice_resample_coeffs.inc"
};
static int16_t quantise(float x)
{
    if (x > 32767) return 32767;
    if (x < -32768) return -32768;
    return (int16_t)x;
}
size_t voice_downsample(voice_downsample_t *s, const int16_t *in, size_t n, int16_t *out)
{
    size_t used = 0;
    for (size_t i = 0; i < n; ++i) {
        s->hist[s->pos] = in[i];
        s->pos = (s->pos + 1) % VOICE_RESAMPLE_TAPS;
        if (++s->phase != 6) continue;
        s->phase = 0;
        float sum = 0; unsigned pos = s->pos;
        for (unsigned j = 0; j < VOICE_RESAMPLE_TAPS; ++j) {
            pos = pos ? pos - 1 : VOICE_RESAMPLE_TAPS - 1;
            sum += s->hist[pos] * coefficients[j];
        }
        out[used++] = quantise(sum);
    }
    return used;
}
size_t voice_upsample(voice_upsample_t *s, const int16_t *in, size_t n, int16_t *out)
{
    const unsigned taps = VOICE_RESAMPLE_TAPS / 6;
    for (size_t i = 0; i < n; ++i) {
        s->hist[s->pos] = in[i]; s->pos = (s->pos + 1) % taps;
        for (unsigned phase = 0; phase < 6; ++phase) {
            float sum = 0; unsigned pos = s->pos;
            for (unsigned j = 0; j < taps; ++j) {
                pos = pos ? pos - 1 : taps - 1;
                sum += s->hist[pos] * coefficients[phase + 6 * j];
            }
            out[6 * i + phase] = quantise(sum * 6);
        }
    }
    return n * 6;
}
