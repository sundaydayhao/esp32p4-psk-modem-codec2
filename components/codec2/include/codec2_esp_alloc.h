#pragma once

#include <stdlib.h>
#include "esp_heap_caps.h"

/* Included only while compiling the ESP-IDF Codec2 component. Codec2 and
 * Kiss FFT use CPU-only buffers; reserve internal RAM for task stacks/DMA.
 * Include stdlib before replacing allocation names, preserving its ABI. */
static inline void *codec2_esp_malloc(size_t size)
{
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static inline void *codec2_esp_calloc(size_t count, size_t size)
{
    return heap_caps_calloc(count, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static inline void *codec2_esp_realloc(void *ptr, size_t size)
{
    return heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

#define malloc codec2_esp_malloc
#define calloc codec2_esp_calloc
#define realloc codec2_esp_realloc
