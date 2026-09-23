/*
 * host_voice_loopback.c — PC 实时回环测试工具
 *
 * 数据流（与本工程 ESP32-P4 板载语音链路完全一致）：
 *
 *   麦克风 48 kHz → 低通降采样 8 kHz → Codec2 700C 编码(28 bit/40 ms)
 *   → 双帧打包成 15 字节语音包(voice_protocol, 同步字+序号+CRC)
 *   → 8PSK1200F / 8PSK1000F 调制(psk_tx, 48 kHz)
 *   → [可选 AWGN 信道] → DC blocker(audio_decim) → 解调(psk_rx, 二进制同步字)
 *   → voice_protocol 解包 → voice_playout 去抖/丢帧隐藏 → Codec2 解码
 *   → 升采样 48 kHz → 扬声器
 *
 * 全部 DSP 复用本工程真实固件源码（components/codec2、components/psk_rtty、
 * main/psk_tx.c、main/psk_rx.c、main/psk_codec.c、main/psk_modes.c、
 * main/voice_*.c、main/audio_decim.c），与板载行为逐块对应。
 *
 * 用法见 --help；无音频设备自测：--selftest N。
 * 构建：tools/build_host_voice_loopback.ps1
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <windows.h>
#include <mmsystem.h>

#ifdef _MSC_VER
#pragma comment(lib, "winmm.lib")
#endif

#include "codec2.h"
#include "psk_modes.h"
#include "psk_tx.h"
#include "psk_rx.h"
#include "audio_decim.h"
#include "voice_protocol.h"
#include "voice_playout.h"
#include "voice_resample.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#define TWO_PI 6.28318530717958647692

#define SR 48000          /* 全链路采样率，与板载一致 */
#define BLK 256           /* DSP 处理块大小（5.33 ms） */
#define MIC_RING_CAP (SR / 2)   /* 麦克风环形缓冲 0.5 s */
#define SPK_RING_CAP (SR * 2)   /* 扬声器环形缓冲 2 s */
#define TX_RING_CAP 2048        /* TX 语音包字节环形缓冲 */
#define A_BLOCK 2400            /* waveIn/waveOut 缓冲 50 ms */
#define A_BUFS 4

typedef struct {
    /* 控制项 */
    volatile LONG stop_requested;
    volatile LONG stop_began;
    volatile LONG encode_enabled;
    psk_mode_t mode;
    int tone_hz;
    int rx_offset_hz;
    double noise_db;
    double noise_std;
    double duration_sec;
    bool selftest;
    int mic_dev;                 /* -1 = WAVE_MAPPER */
    int spk_dev;
    int in_ch, out_ch;           /* 实际打开的通道数 1/2 */
    float mic_gain;
    float out_gain;
    const char *dump_prefix;
    bool list_devices;

    /* 麦克风环形缓冲（waveIn 回调线程写，DSP 线程读） */
    CRITICAL_SECTION mic_lock;
    int16_t mic_ring[MIC_RING_CAP];
    unsigned mic_head, mic_count;

    /* 扬声器环形缓冲（DSP 线程写，播放线程读） */
    CRITICAL_SECTION spk_lock;
    int16_t spk_ring[SPK_RING_CAP];
    unsigned spk_head, spk_count;
    uint32_t spk_underflows, spk_overflows;

    /* TX 语音包字节环形缓冲（DSP 线程单线程使用） */
    uint8_t tx_ring[TX_RING_CAP];
    unsigned tx_rpos, tx_wpos, tx_bytes;
    bool tx_end;

    /* DSP 状态 */
    struct CODEC2 *enc;
    struct CODEC2 *dec;
    psk_tx_t tx;
    psk_rx_t rx;
    voice_wire_rx_t wire;
    voice_playout_t playout;
    voice_downsample_t down;
    voice_upsample_t up;
    audio_decim_state_t decim;
    voice_record_t pending;
    uint8_t next_seq;
    int16_t frame8[VOICE_FRAME_SAMPLES];
    unsigned frame8_used;

    /* 统计 */
    uint64_t sent_packets;
    uint64_t rx_packets, rx_frames, crc_errors, bad_desc, sync_drops;
    uint64_t decoded_frames, concealed_frames;
    uint32_t mic_zero_blocks;
    uint32_t t_start_ms;
    uint32_t last_status_ms;

    /* 端到端延迟跟踪（record 序号 -> 编码时刻 ms） */
    uint32_t enc_ms[256];
    uint8_t enc_ms_valid[256];
    uint64_t lat_sum;
    uint32_t lat_count, lat_max;

    /* AWGN */
    uint64_t rng;
    double gz0, gz1;
    bool gz_ready;

    uint64_t total_samples;
    uint64_t stop_req_samples;

    /* 调试 WAV */
    FILE *wav_tx, *wav_rxin, *wav_rxout;

    /* 实时音频句柄（非自测模式） */
    HWAVEIN hIn;
    HWAVEOUT hOut;
    WAVEHDR in_hdr[A_BUFS];
    WAVEHDR out_hdr[A_BUFS];
    int16_t *in_bufs[A_BUFS];
    int16_t *out_bufs[A_BUFS];   /* 立体声时容量为 A_BLOCK*2 */
    int16_t cap_downmix[A_BLOCK];
    HANDLE hPlayEvent;
    HANDLE hPlayThread;
    volatile LONG play_busy[A_BUFS];
    volatile LONG dsp_done;
} app_t;

static app_t g_app;

/* ------------------------------------------------------------------ */
/* 时间与工具函数                                                       */
/* ------------------------------------------------------------------ */

static double qpc_now(void)
{
    static LARGE_INTEGER freq;
    static BOOL have_freq = FALSE;
    LARGE_INTEGER c;
    if (!have_freq) {
        QueryPerformanceFrequency(&freq);
        have_freq = TRUE;
    }
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)freq.QuadPart;
}

static uint32_t now_ms(app_t *a)
{
    if (a->selftest) {
        /* 自测满速运行，播放时钟由虚拟采样时钟驱动 */
        return (uint32_t)(a->total_samples * 1000 / SR);
    }
    return GetTickCount();
}

static int16_t clamp16(double v)
{
    if (v > 32767.0) return 32767;
    if (v < -32768.0) return -32768;
    return (int16_t)v;
}

/* xorshift64 PRNG */
static uint64_t rng_next(app_t *a)
{
    uint64_t x = a->rng;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    a->rng = x;
    return x;
}

/* 标准正态分布（Box-Muller） */
static double gaussian(app_t *a)
{
    if (a->gz_ready) {
        a->gz_ready = false;
        return a->gz1;
    }
    double u1, u2;
    do {
        u1 = (double)(rng_next(a) >> 11) / (double)(1ULL << 53);
    } while (u1 <= 1e-12);
    u2 = (double)(rng_next(a) >> 11) / (double)(1ULL << 53);
    const double r = sqrt(-2.0 * log(u1));
    a->gz0 = r * cos(TWO_PI * u2);
    a->gz1 = r * sin(TWO_PI * u2);
    a->gz_ready = true;
    return a->gz0;
}

/* ------------------------------------------------------------------ */
/* 环形缓冲（加锁）                                                     */
/* ------------------------------------------------------------------ */

static void ring_put_mic(app_t *a, const int16_t *s, unsigned n)
{
    if (!n) return;
    EnterCriticalSection(&a->mic_lock);
    for (unsigned i = 0; i < n; ++i) {
        a->mic_ring[(a->mic_head + a->mic_count) % MIC_RING_CAP] = s[i];
        if (a->mic_count < MIC_RING_CAP) ++a->mic_count;
    }
    LeaveCriticalSection(&a->mic_lock);
}

static unsigned ring_get_mic(app_t *a, int16_t *s, unsigned n)
{
    unsigned got = 0;
    EnterCriticalSection(&a->mic_lock);
    while (got < n && a->mic_count) {
        s[got++] = a->mic_ring[a->mic_head];
        a->mic_head = (a->mic_head + 1) % MIC_RING_CAP;
        --a->mic_count;
    }
    LeaveCriticalSection(&a->mic_lock);
    return got;
}

static void ring_put_spk(app_t *a, const int16_t *s, unsigned n)
{
    if (!n) return;
    EnterCriticalSection(&a->spk_lock);
    for (unsigned i = 0; i < n; ++i) {
        if (a->spk_count >= SPK_RING_CAP) {
            ++a->spk_overflows;
            continue;
        }
        a->spk_ring[(a->spk_head + a->spk_count) % SPK_RING_CAP] = s[i];
        ++a->spk_count;
    }
    LeaveCriticalSection(&a->spk_lock);
}

static unsigned ring_get_spk(app_t *a, int16_t *s, unsigned n)
{
    unsigned got = 0;
    EnterCriticalSection(&a->spk_lock);
    while (got < n && a->spk_count) {
        s[got++] = a->spk_ring[a->spk_head];
        a->spk_head = (a->spk_head + 1) % SPK_RING_CAP;
        --a->spk_count;
    }
    LeaveCriticalSection(&a->spk_lock);
    return got;
}

static unsigned ring_avail_spk(app_t *a)
{
    unsigned n;
    EnterCriticalSection(&a->spk_lock);
    n = a->spk_count;
    LeaveCriticalSection(&a->spk_lock);
    return n;
}

/* TX 字节环形缓冲（仅 DSP 线程） */
static void txring_put(app_t *a, const uint8_t *d, unsigned n)
{
    for (unsigned i = 0; i < n; ++i) {
        if (a->tx_bytes >= TX_RING_CAP) {
            /* 极端情况下丢弃最旧字节；连续语音下 TX 消耗快于产生，不会触发 */
            a->tx_rpos = (a->tx_rpos + 1) % TX_RING_CAP;
            --a->tx_bytes;
        }
        a->tx_ring[a->tx_wpos] = d[i];
        a->tx_wpos = (a->tx_wpos + 1) % TX_RING_CAP;
        ++a->tx_bytes;
    }
}

static int txring_get(app_t *a)
{
    if (a->tx_bytes == 0) return -1;
    const int b = a->tx_ring[a->tx_rpos];
    a->tx_rpos = (a->tx_rpos + 1) % TX_RING_CAP;
    --a->tx_bytes;
    return b;
}

/* ------------------------------------------------------------------ */
/* WAV 调试输出                                                         */
/* ------------------------------------------------------------------ */

static FILE *wav_open(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) return NULL;
    static const uint8_t hdr[44] = {
        'R','I','F','F', 0,0,0,0, 'W','A','V','E',
        'f','m','t',' ', 16,0,0,0, 1,0, 1,0,
        0x80,0xbb,0,0, 0x00,0x77,0x01,0, 2,0, 16,0,
        'd','a','t','a', 0,0,0,0
    };
    fwrite(hdr, 1, sizeof(hdr), f);
    return f;
}

static void wav_write(FILE *f, const int16_t *s, size_t n)
{
    if (f) fwrite(s, sizeof(int16_t), n, f);
}

static void wav_close(FILE *f)
{
    if (!f) return;
    const long data = ftell(f) - 44;
    const long riff = data + 36;
    fseek(f, 4, SEEK_SET);
    const uint32_t rv = (uint32_t)riff;
    fwrite(&rv, 4, 1, f);
    fseek(f, 40, SEEK_SET);
    const uint32_t dv = (uint32_t)data;
    fwrite(&dv, 4, 1, f);
    fclose(f);
}

/* ------------------------------------------------------------------ */
/* 信道（AWGN；频偏通过 RX 调谐偏移模拟，走 AFC）                        */
/* ------------------------------------------------------------------ */

static void apply_channel(app_t *a, int16_t *buf, size_t n)
{
    if (a->noise_std <= 0.0) return;
    for (size_t i = 0; i < n; ++i) {
        const double v = (double)buf[i] + gaussian(a) * a->noise_std;
        buf[i] = clamp16(v);
    }
}

/* ------------------------------------------------------------------ */
/* TX 侧                                                               */
/* ------------------------------------------------------------------ */

/* psk_tx 二进制流取字节回调：-2 = 结束，0 = 空闲字节 */
static int tx_fetch(void *user)
{
    app_t *a = user;
    const int b = txring_get(a);
    if (b < 0) return a->tx_end ? -2 : 0;
    return b;
}

static void enqueue_record(app_t *a, bool end)
{
    voice_record_t *r = &a->pending;
    if (r->count == 0 && !end) return;
    if (end) r->flags |= VOICE_FLAG_END;
    r->sequence = a->next_seq++;

    voice_packet_t pkt;
    if (!voice_encode(r, &pkt) || pkt.len == 0) {
        r->count = 0;
        r->flags = 0;
        return;
    }
    txring_put(a, pkt.data, (unsigned)pkt.len);
    a->enc_ms[r->sequence & 0xff] = now_ms(a);
    a->enc_ms_valid[r->sequence & 0xff] = 1;
    ++a->sent_packets;
    r->count = 0;
    r->flags = 0;
}

static void encode_frame(app_t *a, int16_t *pcm8k)
{
    if (!a->enc) return;
    if (a->pending.count >= 2) enqueue_record(a, false);
    codec2_encode(a->enc, a->pending.frames[a->pending.count++], pcm8k);
    if (a->pending.count == 2) enqueue_record(a, false);
}

/* ------------------------------------------------------------------ */
/* RX 侧                                                               */
/* ------------------------------------------------------------------ */

static void on_voice_record(const voice_record_t *r, void *user)
{
    app_t *a = user;
    const uint32_t now = now_ms(a);
    if (a->enc_ms_valid[r->sequence & 0xff]) {
        const uint32_t d = now - a->enc_ms[r->sequence & 0xff];
        a->lat_sum += d;
        ++a->lat_count;
        if (d > a->lat_max) a->lat_max = d;
        a->enc_ms_valid[r->sequence & 0xff] = 0;
    }
    voice_playout_push(&a->playout, r, now);
}

/* ------------------------------------------------------------------ */
/* 自测信号源                                                           */
/* ------------------------------------------------------------------ */

static void synth_fill(app_t *a, int16_t *buf, unsigned n)
{
    for (unsigned i = 0; i < n; ++i) {
        const uint64_t s = a->total_samples + i;
        const double t = (double)s / SR;
        double v = 0.50 * sin(TWO_PI * 180.0 * t)
                 + 0.30 * sin(TWO_PI * 440.0 * t)
                 + 0.20 * sin(TWO_PI * 880.0 * t);
        v += gaussian(a) * 0.02;
        const double env = 0.75 + 0.25 * sin(TWO_PI * 0.5 * t);
        buf[i] = clamp16(v * env * 26000.0);
    }
}

/* ------------------------------------------------------------------ */
/* DSP 单步（实时与自测共用）                                           */
/* ------------------------------------------------------------------ */

static void dsp_step(app_t *a)
{
    int16_t in48[BLK];

    if (a->selftest) {
        synth_fill(a, in48, BLK);
    } else {
        unsigned got = ring_get_mic(a, in48, BLK);
        if (got == 0) ++a->mic_zero_blocks;
        for (unsigned i = got; i < BLK; ++i) in48[i] = 0;
        if (a->mic_gain != 1.0f && got) {
            for (unsigned i = 0; i < got; ++i)
                in48[i] = clamp16((double)in48[i] * a->mic_gain);
        }
    }

    /* 48k -> 8k 降采样并攒 Codec2 帧 */
    int16_t s8[VOICE_FRAME_SAMPLES];
    const size_t n8 = voice_downsample(&a->down, in48, BLK, s8);
    if (a->encode_enabled) {
        for (size_t i = 0; i < n8; ++i) {
            a->frame8[a->frame8_used++] = s8[i];
            if (a->frame8_used == VOICE_FRAME_SAMPLES) {
                a->frame8_used = 0;
                encode_frame(a, a->frame8);
            }
        }
    }

    /* 调制 -> 信道 -> DC blocker -> 解调（与板载 RX 输入路径一致） */
    int16_t txbuf[BLK], rxbuf[BLK];
    size_t n = psk_tx_generate(&a->tx, txbuf, BLK);
    if (n < BLK) memset(txbuf + n, 0, (BLK - n) * sizeof(int16_t));
    apply_channel(a, txbuf, BLK);
    (void)audio_decim_process(&a->decim, txbuf, BLK, rxbuf, 1);

    char bytes[512];
    const size_t nb = psk_rx_process(&a->rx, rxbuf, BLK, bytes, sizeof(bytes));
    if (nb) voice_wire_feed(&a->wire, (const uint8_t *)bytes, nb, on_voice_record, a);

    /* 播放调度 -> Codec2 解码 -> 升采样 -> 扬声器 */
    uint8_t bits[VOICE_FRAME_BYTES];
    const int kind = voice_playout_next(&a->playout, now_ms(a), bits);
    if (kind) {
        int16_t out8[VOICE_FRAME_SAMPLES];
        if (kind == 1) codec2_decode(a->dec, out8, bits);
        else memset(out8, 0, sizeof(out8));
        int16_t out48[VOICE_FRAME_SAMPLES * 6];
        const size_t n48 = voice_upsample(&a->up, out8, VOICE_FRAME_SAMPLES, out48);
        if (a->out_gain != 1.0f) {
            for (size_t i = 0; i < n48; ++i)
                out48[i] = clamp16((double)out48[i] * a->out_gain);
        }
        if (!a->selftest) ring_put_spk(a, out48, (unsigned)n48);
        if (kind == 1) ++a->decoded_frames;
        else ++a->concealed_frames;
        if (a->wav_rxout) wav_write(a->wav_rxout, out48, n48);
    }

    if (a->wav_tx) wav_write(a->wav_tx, txbuf, BLK);
    if (a->wav_rxin) wav_write(a->wav_rxin, rxbuf, BLK);

    a->rx_packets = a->wire.packets;
    a->rx_frames = a->wire.frames;
    a->crc_errors = a->wire.crc_errors;
    a->bad_desc = a->wire.bad_descriptors;
    a->sync_drops = a->wire.sync_drops;
    a->total_samples += BLK;
}

/* ------------------------------------------------------------------ */
/* 状态打印                                                             */
/* ------------------------------------------------------------------ */

static void print_status(app_t *a)
{
    const uint32_t now = now_ms(a);
    if ((uint32_t)(now - a->last_status_ms) < 1000) return;
    a->last_status_ms = now;

    psk_rx_debug_t d;
    psk_rx_debug_snapshot(&a->rx, &d);

    const double lat_avg = a->lat_count ? (double)a->lat_sum / a->lat_count : 0.0;
    /* 近似端到端：编码->RX 解包 + 播放缓冲(80ms) + 扬声器缓冲(50ms) + 麦克风块(40ms) */
    const double e2e = lat_avg + 170.0;
    const double t = (double)a->total_samples / SR;

    printf("\r[%s @%dHz] t=%6.1fs | TXpkt=%llu RXpkt=%llu/%llu crc=%llu bad=%llu sync=%llu"
           " | dec=%llu conc=%llu drop=%llu lost=%llu dup=%llu | lock=%u ber=%.0f%% "
           "| wirelat=%u max=%u e2e~%.0fms | mic0=%u   ",
           psk_mode_name(a->mode), a->tone_hz, t,
           (unsigned long long)a->sent_packets,
           (unsigned long long)a->rx_packets, (unsigned long long)a->rx_frames,
           (unsigned long long)a->crc_errors, (unsigned long long)a->bad_desc,
           (unsigned long long)a->sync_drops,
           (unsigned long long)a->decoded_frames, (unsigned long long)a->concealed_frames,
           (unsigned long long)a->playout.dropped_frames,
           (unsigned long long)a->playout.lost_packets, (unsigned long long)a->playout.duplicates,
           d.lock_state,
           a->rx_frames ? 100.0 * (double)a->crc_errors / (double)a->rx_frames : 0.0,
           (unsigned)(lat_avg + 0.5), (unsigned)a->lat_max, e2e,
           a->mic_zero_blocks);
    fflush(stdout);
}

static void print_summary(app_t *a)
{
    const double lat_avg = a->lat_count ? (double)a->lat_sum / a->lat_count : 0.0;
    printf("\n=== host_voice_loopback 汇总 ===\n");
    printf("模式            : %s @ %d Hz (RX 调谐 %+d Hz, AFC %s)\n",
           psk_mode_name(a->mode), a->tone_hz, a->rx_offset_hz,
           a->rx.config.afc_enabled ? "on" : "off");
    printf("时长            : %.2f s\n", (double)a->total_samples / SR);
    printf("语音包          : 发送 %llu / 接收 %llu（帧 %llu）\n",
           (unsigned long long)a->sent_packets,
           (unsigned long long)a->rx_packets, (unsigned long long)a->rx_frames);
    printf("链路质量        : CRC 错误 %llu, 坏描述符 %llu, 同步丢失字节 %llu, 丢包 %llu, 重复 %llu\n",
           (unsigned long long)a->crc_errors, (unsigned long long)a->bad_desc,
           (unsigned long long)a->sync_drops,
           (unsigned long long)a->playout.lost_packets,
           (unsigned long long)a->playout.duplicates);
    printf("Codec2          : 解码帧 %llu, 隐藏帧 %llu, 播放缓冲丢弃帧 %llu（启动突发属正常现象）\n",
           (unsigned long long)a->decoded_frames, (unsigned long long)a->concealed_frames,
           (unsigned long long)a->playout.dropped_frames);
    if (!a->selftest)
        printf("扬声器          : 欠载 %u, 溢出 %u\n", a->spk_underflows, a->spk_overflows);
    if (a->lat_count)
        printf("编码->解包延迟  : 平均 %.1f ms, 最大 %u ms（估计端到端 ≈ %.0f ms）\n",
               lat_avg, a->lat_max, lat_avg + 170.0);
    if (a->selftest) {
        const bool ok = a->rx_packets == a->sent_packets && a->crc_errors == 0 &&
                        a->decoded_frames > 0 &&
                        a->concealed_frames <= a->decoded_frames + 2;
        printf("自测结果        : %s\n", ok ? "PASS" : "FAIL");
        printf("  - 包数一致          : %s (%llu/%llu)\n",
               a->rx_packets == a->sent_packets ? "是" : "否",
               (unsigned long long)a->rx_packets, (unsigned long long)a->sent_packets);
        printf("  - CRC 全对          : %s (%llu)\n",
               a->crc_errors == 0 ? "是" : "否", (unsigned long long)a->crc_errors);
        printf("  - 有解码输出        : %s (%llu 帧)\n",
               a->decoded_frames > 0 ? "是" : "否", (unsigned long long)a->decoded_frames);
    }
}

/* ------------------------------------------------------------------ */
/* 实时音频（WinMM）                                                    */
/* ------------------------------------------------------------------ */

static void CALLBACK cap_cb(HWAVEIN h, UINT msg, DWORD_PTR inst, DWORD_PTR p1, DWORD_PTR p2)
{
    (void)h;
    (void)p2;
    if (msg != WIM_DATA) return;
    app_t *a = (app_t *)inst;
    WAVEHDR *hd = (WAVEHDR *)p1;
    const int16_t *s = (const int16_t *)hd->lpData;
    size_t n = hd->dwBytesRecorded / sizeof(int16_t);
    if (a->in_ch == 2) {
        const size_t frames = n / 2;
        for (size_t i = 0; i < frames && i < A_BLOCK; ++i) {
            const int32_t v = ((int32_t)s[2 * i] + (int32_t)s[2 * i + 1]) / 2;
            a->cap_downmix[i] = (int16_t)v;
        }
        ring_put_mic(a, a->cap_downmix, (unsigned)frames);
    } else {
        ring_put_mic(a, s, (unsigned)n);
    }
    if (!a->stop_requested) waveInAddBuffer(a->hIn, hd, sizeof(WAVEHDR));
}

static void CALLBACK play_cb(HWAVEOUT h, UINT msg, DWORD_PTR inst, DWORD_PTR p1, DWORD_PTR p2)
{
    (void)h;
    (void)p2;
    if (msg != WOM_DONE) return;
    app_t *a = (app_t *)inst;
    const WAVEHDR *hd = (const WAVEHDR *)p1;
    for (int i = 0; i < A_BUFS; ++i) {
        if ((const WAVEHDR *)a->out_hdr[i].dwUser == hd) {
            InterlockedExchange(&a->play_busy[i], 0);
            break;
        }
    }
    SetEvent(a->hPlayEvent);
}

static DWORD WINAPI playback_thread(LPVOID arg)
{
    app_t *a = arg;
    for (;;) {
        int free_idx = -1;
        for (int i = 0; i < A_BUFS; ++i) {
            if (!a->play_busy[i]) { free_idx = i; break; }
        }
        if (free_idx < 0) {
            WaitForSingleObject(a->hPlayEvent, 20);
            continue;
        }
        const bool ending = a->stop_requested && a->dsp_done;
        unsigned n = ring_get_spk(a, a->out_bufs[free_idx], A_BLOCK);
        if (ending && n == 0 && ring_avail_spk(a) == 0) {
            bool any_busy = false;
            for (int i = 0; i < A_BUFS; ++i)
                if (a->play_busy[i]) { any_busy = true; break; }
            if (!any_busy) break;
            WaitForSingleObject(a->hPlayEvent, 20);
            continue;
        }
        if (n < A_BLOCK) {
            if (!ending) ++a->spk_underflows;
            memset(a->out_bufs[free_idx] + n, 0, (A_BLOCK - n) * sizeof(int16_t));
        }
        unsigned bytes = A_BLOCK * sizeof(int16_t);
        if (a->out_ch == 2) {
            /* 单声道 -> 立体声复制 */
            for (int i = A_BLOCK - 1; i >= 0; --i) {
                const int16_t v = a->out_bufs[free_idx][i];
                a->out_bufs[free_idx][2 * i] = v;
                a->out_bufs[free_idx][2 * i + 1] = v;
            }
            bytes *= 2;
        }
        a->out_hdr[free_idx].dwBufferLength = bytes;
        a->out_hdr[free_idx].dwFlags = 0;
        InterlockedExchange(&a->play_busy[free_idx], 1);
        if (waveOutWrite(a->hOut, &a->out_hdr[free_idx], sizeof(WAVEHDR)) != MMSYSERR_NOERROR) {
            InterlockedExchange(&a->play_busy[free_idx], 0);
            break;
        }
    }
    return 0;
}

/* 尝试以给定通道数打开 waveIn/waveOut */
static bool open_audio(app_t *a)
{
    if (a->list_devices || a->selftest) return true;

    WAVEFORMATEX fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.wFormatTag = WAVE_FORMAT_PCM;
    fmt.nSamplesPerSec = SR;
    fmt.wBitsPerSample = 16;

    const UINT mic_dev = a->mic_dev < 0 ? WAVE_MAPPER : (UINT)a->mic_dev;
    const UINT spk_dev = a->spk_dev < 0 ? WAVE_MAPPER : (UINT)a->spk_dev;

    bool in_ok = false, out_ok = false;
    for (int ch = 1; ch <= 2 && !in_ok; ++ch) {
        fmt.nChannels = (WORD)ch;
        fmt.nBlockAlign = (WORD)(ch * 2);
        fmt.nAvgBytesPerSec = SR * ch * 2;
        if (waveInOpen(&a->hIn, mic_dev, &fmt, (DWORD_PTR)cap_cb, (DWORD_PTR)a,
                       CALLBACK_FUNCTION) == MMSYSERR_NOERROR) {
            in_ok = true;
            a->in_ch = ch;
        }
    }
    for (int ch = 1; ch <= 2 && !out_ok; ++ch) {
        fmt.nChannels = (WORD)ch;
        fmt.nBlockAlign = (WORD)(ch * 2);
        fmt.nAvgBytesPerSec = SR * ch * 2;
        if (waveOutOpen(&a->hOut, spk_dev, &fmt, (DWORD_PTR)play_cb, (DWORD_PTR)a,
                        CALLBACK_FUNCTION) == MMSYSERR_NOERROR) {
            out_ok = true;
            a->out_ch = ch;
        }
    }
    if (!in_ok || !out_ok) {
        fprintf(stderr, "无法以 48 kHz 16-bit 打开音频设备（mic=%s spk=%s）。\n"
                        "请检查设备是否被占用；运行 --list-devices 查看可用设备。\n",
                in_ok ? "ok" : "FAIL", out_ok ? "ok" : "FAIL");
        return false;
    }

    const size_t out_cap = (size_t)A_BLOCK * (a->out_ch == 2 ? 2 : 1);
    for (int i = 0; i < A_BUFS; ++i) {
        a->in_bufs[i] = (int16_t *)malloc(A_BLOCK * sizeof(int16_t));
        a->out_bufs[i] = (int16_t *)malloc(out_cap * sizeof(int16_t));
        if (!a->in_bufs[i] || !a->out_bufs[i]) {
            fprintf(stderr, "内存不足\n");
            return false;
        }
        memset(&a->in_hdr[i], 0, sizeof(WAVEHDR));
        memset(&a->out_hdr[i], 0, sizeof(WAVEHDR));
        a->in_hdr[i].lpData = (LPSTR)a->in_bufs[i];
        a->in_hdr[i].dwBufferLength = A_BLOCK * sizeof(int16_t);
        a->out_hdr[i].lpData = (LPSTR)a->out_bufs[i];
        a->out_hdr[i].dwBufferLength = A_BLOCK * sizeof(int16_t);
        a->out_hdr[i].dwUser = (DWORD_PTR)&a->out_hdr[i];
        waveInPrepareHeader(a->hIn, &a->in_hdr[i], sizeof(WAVEHDR));
        waveOutPrepareHeader(a->hOut, &a->out_hdr[i], sizeof(WAVEHDR));
        waveInAddBuffer(a->hIn, &a->in_hdr[i], sizeof(WAVEHDR));
    }
    waveInStart(a->hIn);

    a->hPlayEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    a->hPlayThread = CreateThread(NULL, 0, playback_thread, a, 0, NULL);
    if (!a->hPlayEvent || !a->hPlayThread) {
        fprintf(stderr, "播放线程创建失败\n");
        return false;
    }
    return true;
}

static void close_audio(app_t *a)
{
    if (a->selftest || a->list_devices) return;
    if (a->hIn) {
        waveInStop(a->hIn);
        waveInReset(a->hIn);
    }
    /* 先让播放线程自然排空，再复位/关闭设备 */
    InterlockedExchange(&a->dsp_done, 1);
    if (a->hPlayThread) {
        if (WaitForSingleObject(a->hPlayThread, 5000) != WAIT_OBJECT_0) {
            if (a->hOut) waveOutReset(a->hOut);
            WaitForSingleObject(a->hPlayThread, 1000);
        }
        CloseHandle(a->hPlayThread);
    }
    if (a->hPlayEvent) CloseHandle(a->hPlayEvent);
    if (a->hIn) {
        for (int i = 0; i < A_BUFS; ++i) {
            waveInUnprepareHeader(a->hIn, &a->in_hdr[i], sizeof(WAVEHDR));
            free(a->in_bufs[i]);
        }
        waveInClose(a->hIn);
    }
    if (a->hOut) {
        waveOutReset(a->hOut);
        for (int i = 0; i < A_BUFS; ++i) {
            waveOutUnprepareHeader(a->hOut, &a->out_hdr[i], sizeof(WAVEHDR));
            free(a->out_bufs[i]);
        }
        waveOutClose(a->hOut);
    }
}

/* ------------------------------------------------------------------ */
/* 键盘 / 控制台                                                        */
/* ------------------------------------------------------------------ */

static BOOL WINAPI ctrl_handler(DWORD type)
{
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
        InterlockedExchange(&g_app.stop_requested, 1);
        return TRUE;
    }
    return FALSE;
}

static void poll_key(app_t *a)
{
    if (a->selftest) return;
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    if (h == INVALID_HANDLE_VALUE || h == NULL) return;
    INPUT_RECORD rec;
    DWORD n = 0;
    while (PeekConsoleInput(h, &rec, 1, &n) && n) {
        if (!ReadConsoleInput(h, &rec, 1, &n) || !n) break;
        if (rec.EventType == KEY_EVENT && rec.Event.KeyEvent.bKeyDown) {
            const char c = rec.Event.KeyEvent.uChar.AsciiChar;
            if (c == 'q' || c == 'Q') {
                InterlockedExchange(&a->stop_requested, 1);
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* 设备列表                                                             */
/* ------------------------------------------------------------------ */

static void list_devices(void)
{
    printf("=== 录音设备 (waveIn) ===\n");
    const UINT nin = waveInGetNumDevs();
    for (UINT i = 0; i < nin; ++i) {
        WAVEINCAPS caps;
        if (waveInGetDevCaps(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR)
            printf("  mic %u : %s\n", i, (const char *)caps.szPname);
    }
    printf("=== 播放设备 (waveOut) ===\n");
    const UINT nout = waveOutGetNumDevs();
    for (UINT i = 0; i < nout; ++i) {
        WAVEOUTCAPS caps;
        if (waveOutGetDevCaps(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR)
            printf("  spk %u : %s\n", i, (const char *)caps.szPname);
    }
}

/* ------------------------------------------------------------------ */
/* 选项解析                                                             */
/* ------------------------------------------------------------------ */

static void usage(const char *prog)
{
    printf(
        "用法: %s [选项]\n"
        "\n"
        "实时回环：麦克风 -> Codec2 700C -> 8PSK 调制 -> [信道] -> 解调 -> Codec2 -> 扬声器\n"
        "所有 DSP 复用本工程固件源码，链路与 ESP32-P4 板载语音完全一致。\n"
        "\n"
        "选项:\n"
        "  --mode NAME          PSK 模式（默认 8PSK1200F；语音支持 8PSK1200F / 8PSK1000F）\n"
        "  --tone HZ            TX 载波频率（默认 2000）\n"
        "  --rx-offset HZ       RX 调谐偏移（模拟频偏，走 AFC，默认 0，|offset|<=100）\n"
        "  --noise-db X         信道 AWGN，X 为相对音调 RMS 的信噪比 dB（默认关闭）\n"
        "  --mic N / --speaker N  音频设备索引（默认系统默认设备；--list-devices 查看）\n"
        "  --mic-gain-db X      麦克风增益 dB（默认 0）\n"
        "  --output-gain X      扬声器输出增益 0..2（默认 1.0）\n"
        "  --duration SEC       运行 SEC 秒后自动停止（默认持续运行，按 q 或 Ctrl+C 停止）\n"
        "  --dump PREFIX        输出 PREFIX_tx.wav(已调制) PREFIX_rx_in.wav(接收信号)\n"
        "                       PREFIX_rx_out.wav(还原语音)\n"
        "  --selftest SEC       无音频设备自测：合成语音跑通全链路 SEC 秒并打印 PASS/FAIL\n"
        "  --list-devices       列出音频设备后退出\n"
        "  --help               显示本帮助\n"
        "\n"
        "提示: 实时模式请佩戴耳机，避免扬声器语音回灌麦克风形成回声环。\n",
        prog);
}

static double parse_double(const char *s, const char *name, double lo, double hi)
{
    char *end = NULL;
    const double v = strtod(s, &end);
    if (!end || *end != '\0' || v < lo || v > hi) {
        fprintf(stderr, "无效 %s: %s（范围 [%g, %g]）\n", name, s, lo, hi);
        exit(2);
    }
    return v;
}

static int parse_int(const char *s, const char *name)
{
    char *end = NULL;
    const long v = strtol(s, &end, 10);
    if (!end || *end != '\0') {
        fprintf(stderr, "无效 %s: %s\n", name, s);
        exit(2);
    }
    return (int)v;
}

static void parse_args(app_t *a, int argc, char **argv)
{
    for (int i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        if (!strcmp(arg, "--help") || !strcmp(arg, "-h")) {
            usage(argv[0]);
            exit(0);
        } else if (!strcmp(arg, "--list-devices")) {
            a->list_devices = true;
        } else if (!strcmp(arg, "--mode")) {
            if (++i >= argc) { fprintf(stderr, "--mode 需要参数\n"); exit(2); }
            if (!strcmp(argv[i], "8PSK1200F")) a->mode = PSK_MODE_8PSK1200F;
            else if (!strcmp(argv[i], "8PSK1000F")) a->mode = PSK_MODE_8PSK1000F;
            else {
                fprintf(stderr, "语音链路仅支持 8PSK1200F / 8PSK1000F（与固件一致），收到: %s\n", argv[i]);
                exit(2);
            }
        } else if (!strcmp(arg, "--tone")) {
            a->tone_hz = parse_int(argv[++i], "--tone");
            if (a->tone_hz < 300 || a->tone_hz > 3500) { fprintf(stderr, "--tone 范围 300..3500\n"); exit(2); }
        } else if (!strcmp(arg, "--rx-offset")) {
            a->rx_offset_hz = parse_int(argv[++i], "--rx-offset");
            if (abs(a->rx_offset_hz) > 100) { fprintf(stderr, "--rx-offset 范围 [-100,100]\n"); exit(2); }
        } else if (!strcmp(arg, "--noise-db")) {
            a->noise_db = parse_double(argv[++i], "--noise-db", 0.0, 60.0);
        } else if (!strcmp(arg, "--mic")) {
            a->mic_dev = parse_int(argv[++i], "--mic");
            if (a->mic_dev < 0) { fprintf(stderr, "--mic 不能为负\n"); exit(2); }
        } else if (!strcmp(arg, "--speaker")) {
            a->spk_dev = parse_int(argv[++i], "--speaker");
            if (a->spk_dev < 0) { fprintf(stderr, "--speaker 不能为负\n"); exit(2); }
        } else if (!strcmp(arg, "--mic-gain-db")) {
            const double g = parse_double(argv[++i], "--mic-gain-db", -40.0, 40.0);
            a->mic_gain = (float)pow(10.0, g / 20.0);
        } else if (!strcmp(arg, "--output-gain")) {
            a->out_gain = (float)parse_double(argv[++i], "--output-gain", 0.0, 2.0);
        } else if (!strcmp(arg, "--duration")) {
            a->duration_sec = parse_double(argv[++i], "--duration", 0.1, 86400.0);
        } else if (!strcmp(arg, "--dump")) {
            if (++i >= argc) { fprintf(stderr, "--dump 需要参数\n"); exit(2); }
            a->dump_prefix = argv[i];
        } else if (!strcmp(arg, "--selftest")) {
            a->selftest = true;
            a->duration_sec = parse_double(argv[++i], "--selftest", 2.0, 3600.0);
        } else {
            fprintf(stderr, "未知选项: %s（--help 查看用法）\n", arg);
            exit(2);
        }
    }
}

/* ------------------------------------------------------------------ */
/* 停止流程                                                             */
/* ------------------------------------------------------------------ */

/* 必须在 DSP 线程调用（操作 TX 环形缓冲）。幂等。 */
static void begin_stop(app_t *a)
{
    if (InterlockedExchange(&a->stop_requested, 1)) return;
    InterlockedExchange(&a->encode_enabled, 0);
    a->stop_req_samples = a->total_samples;
    /* 刷尾帧：1 帧 -> 补 END；0 帧 -> 空 END 记录（与 content_end_capture 一致） */
    enqueue_record(a, true);
    a->tx_end = true;
}

/* ------------------------------------------------------------------ */
/* 主流程                                                               */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    app_t *a = &g_app;
    memset(a, 0, sizeof(*a));
    a->mode = PSK_MODE_8PSK1200F;
    a->tone_hz = 2000;
    a->mic_dev = -1;
    a->spk_dev = -1;
    a->mic_gain = 1.0f;
    a->out_gain = 1.0f;
    a->rng = ((uint64_t)time(NULL) * 2654435761u) ^ (uint64_t)GetTickCount() ^ 0x9E3779B97F4A7C15ull;

    parse_args(a, argc, argv);

    if (a->list_devices) {
        list_devices();
        return 0;
    }

    if (a->noise_db > 0.0)
        a->noise_std = 26000.0 / sqrt(2.0) * pow(10.0, -a->noise_db / 20.0);

    /* 打开 WAV 调试输出 */
    if (a->dump_prefix) {
        char path[512];
        snprintf(path, sizeof(path), "%s_tx.wav", a->dump_prefix);
        a->wav_tx = wav_open(path);
        snprintf(path, sizeof(path), "%s_rx_in.wav", a->dump_prefix);
        a->wav_rxin = wav_open(path);
        snprintf(path, sizeof(path), "%s_rx_out.wav", a->dump_prefix);
        a->wav_rxout = wav_open(path);
        printf("WAV 调试输出: %s_tx.wav / %s_rx_in.wav / %s_rx_out.wav\n",
               a->dump_prefix, a->dump_prefix, a->dump_prefix);
    }

    /* Codec2 700C：编码器 + 解码器 */
    a->enc = codec2_create(CODEC2_MODE_700C);
    a->dec = codec2_create(CODEC2_MODE_700C);
    if (!a->enc || !a->dec) {
        fprintf(stderr, "Codec2 700C 初始化失败\n");
        return 1;
    }
    if (codec2_samples_per_frame(a->enc) != VOICE_FRAME_SAMPLES ||
        codec2_bits_per_frame(a->enc) != VOICE_FRAME_BITS) {
        fprintf(stderr, "Codec2 参数不匹配（期望 320 样本/28 bit）\n");
        return 1;
    }

    /* 调制器 / 解调器（与板载语音默认一致：8PSK1200F @ 2000 Hz, 48 kHz） */
    psk_tx_init(&a->tx, SR);
    if (psk_tx_start_binary_stream(&a->tx, a->mode, a->tone_hz, tx_fetch, a) != 0) {
        fprintf(stderr, "PSK TX 启动失败\n");
        return 1;
    }
    psk_rx_config_t cfg = {
        .mode = a->mode,
        .sample_rate_hz = SR,
        .rx_tone_hz = a->tone_hz + a->rx_offset_hz,
        .squelch = 20,
        .afc_enabled = 1,
        .squelch_enabled = 0,
    };
    psk_rx_init(&a->rx, &cfg);
    psk_rx_set_binary_sync(&a->rx, VOICE_SYNC_WORD);

    audio_decim_init(&a->decim);
    memset(&a->down, 0, sizeof(a->down));
    memset(&a->up, 0, sizeof(a->up));
    memset(&a->wire, 0, sizeof(a->wire));
    voice_playout_reset(&a->playout);
    a->pending.flags = VOICE_FLAG_START;

    InitializeCriticalSection(&a->mic_lock);
    InitializeCriticalSection(&a->spk_lock);

    if (!a->selftest) {
        if (!open_audio(a)) {
            codec2_destroy(a->enc);
            codec2_destroy(a->dec);
            return 1;
        }
        timeBeginPeriod(1);
        SetConsoleCtrlHandler(ctrl_handler, TRUE);
        printf("host_voice_loopback: 麦克风 -> Codec2 700C -> %s -> Codec2 -> 扬声器\n",
               psk_mode_name(a->mode));
        printf("  音频 48 kHz；载波 %d Hz；RX 调谐 %d Hz%s\n", a->tone_hz,
               a->tone_hz + a->rx_offset_hz,
               a->noise_db > 0.0 ? "; 信道 AWGN" : "");
        printf("  训练约 1.5~2 秒后开始出声。按 q 或 Ctrl+C 停止（佩戴耳机避免回声）。\n");
    } else {
        printf("selftest: 合成语音 -> Codec2 700C -> %s -> Codec2（%.1f 秒虚拟音频，无音频设备）\n",
               psk_mode_name(a->mode), a->duration_sec);
    }

    InterlockedExchange(&a->encode_enabled, 1);
    a->t_start_ms = now_ms(a);
    a->last_status_ms = a->t_start_ms;

    /* pacing: processed_samples/SR ~= real wall-clock time (48 kHz realtime).
     * Do NOT compare absolute QPC time to the sample count - QPC starts at boot. */
    const double pace_t0 = qpc_now();
    uint64_t processed = 0;
    const uint64_t stop_samples = (a->selftest || a->duration_sec > 0.0) ?
        (uint64_t)(a->duration_sec * SR) : 0;

    /* DSP 主循环 */
    for (;;) {
        poll_key(a);

        if (stop_samples && !a->stop_requested && a->total_samples >= stop_samples)
            begin_stop(a);
        if (a->stop_requested) begin_stop(a); /* 幂等；键盘/Ctrl+C 触发的停止在此执行 */

        dsp_step(a);

        if (a->stop_requested) {
            const uint64_t since = a->total_samples - a->stop_req_samples;
            const bool tx_done = !psk_tx_active(&a->tx);
            const bool pl_done = !a->playout.playing;
            if (tx_done && pl_done && since >= SR / 2) break;
            if (since >= SR * 3) break; /* 兜底：最多再跑 3 秒 */
        }

        print_status(a);

        if (!a->selftest) {
            processed += BLK;
            const double elapsed = qpc_now() - pace_t0;
            const double target_elapsed = (double)processed / SR;
            const double dt = target_elapsed - elapsed;
            if (dt > 0.002) Sleep((DWORD)(dt * 1000.0));
        }
    }

    print_summary(a);

    if (a->wav_tx) wav_close(a->wav_tx);
    if (a->wav_rxin) wav_close(a->wav_rxin);
    if (a->wav_rxout) wav_close(a->wav_rxout);

    if (!a->selftest) {
        close_audio(a);
        timeEndPeriod(1);
    }

    codec2_destroy(a->enc);
    codec2_destroy(a->dec);
    DeleteCriticalSection(&a->mic_lock);
    DeleteCriticalSection(&a->spk_lock);

    if (a->selftest) {
        const bool ok = a->rx_packets == a->sent_packets && a->crc_errors == 0 &&
                        a->decoded_frames > 0 &&
                        a->concealed_frames <= a->decoded_frames + 2;
        return ok ? 0 : 1;
    }
    return 0;
}
