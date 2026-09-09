# Codec2 700C component

Vendored from [drowe67/codec2](https://github.com/drowe67/codec2), version 1.2.0,
commit `310777b1c6f1af0bc7c72f5b32f80f6fd9136962` (also recorded in
`UPSTREAM_COMMIT`). `vendor/src` is an unmodified upstream snapshot. The
upstream LGPL-2.1 license is in `vendor/COPYING`; source copyright notices are
preserved. `upstream/`, if present, is an unused retrieval checkout, not a build
input. The firmware uses only the explicit list in `sources.cmake`.

The ESP-IDF wrapper enables only Codec2 700C, uses the portable Kiss FFT, and
does not build FreeDV modems or host command-line programs. The public API is
`codec2_create/encode/decode`, 320 signed PCM samples at 8 kHz and 28 packed bits
per frame. No downloaded binary library, runtime download, or external speech
module is required to build or run.

On ESP32-P4, `include/codec2_esp_alloc.h` is force-included **only for this
component**. It directs Codec2/Kiss FFT CPU buffers to PSRAM, including small
allocations that the global allocator would otherwise place in internal RAM.
The normal ESP-IDF `free()` releases these buffers. The CPU-only Codec2 task's
24 KiB stack also uses PSRAM through the IDF capability-aware task API; its TCB,
other task stacks and DMA buffers stay in internal RAM. The Codec2 task must not
perform Flash/NVS operations. Host builds use the normal C allocator.
No upstream files or shared SDK files are patched.

The initial `codec2_create()` runs inside the 24 KiB Codec2 task, just like later
session resets. Its analysis-window initialization alone uses two 512-element
complex arrays (8 KiB), which cannot fit on the application's 8 KiB main stack.
`content_service_init()` waits for the worker's initialization result before
starting the remaining application tasks.

`generated/` contains codebooks produced by the unmodified upstream
`generate_codebook.c`. They are checked in for reproducible offline cross builds.
Regenerate them with `python tools/prepare_codec2.py` from the project root
(Windows/Cygwin GCC). `include/codec2/version.h` matches the upstream CMake
project version. No Codec2 source modifications were needed.

Future FreeDV 700D/700E work must explicitly add its API, OFDM, LDPC, framing,
resampling and synchronization dependencies and separate real-time tests; merely
linking this speech codec does not implement FreeDV.
