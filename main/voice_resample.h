#pragma once
#include <stddef.h>
#include <stdint.h>
#define VOICE_RESAMPLE_TAPS 144U
typedef struct { int16_t hist[VOICE_RESAMPLE_TAPS]; unsigned pos, phase; } voice_downsample_t;
typedef struct { int16_t hist[VOICE_RESAMPLE_TAPS / 6]; unsigned pos; } voice_upsample_t;
/* Stateful 48 kHz <-> 8 kHz low-pass FIR. Output capacity: ceil(n/6), n*6. */
size_t voice_downsample(voice_downsample_t *s, const int16_t *in, size_t n, int16_t *out);
size_t voice_upsample(voice_upsample_t *s, const int16_t *in, size_t n, int16_t *out);
