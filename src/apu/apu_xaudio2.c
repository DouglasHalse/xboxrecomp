/**
 * XAudio2 Audio Output Backend
 *
 * Provides low-latency audio output via XAudio2 (Win7+).
 * Called from the APU monitor frame to submit mixed samples.
 * Falls back gracefully if XAudio2 is unavailable.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "apu_xaudio2.h"

/* XAudio2 on Windows; elsewhere the same xa2_* interface on SDL2's audio
 * queue (below). */
#if defined(_WIN32)

#define COBJMACROS
#include <windows.h>
#include <xaudio2.h>

#pragma comment(lib, "xaudio2.lib")
#pragma comment(lib, "ole32.lib")

#define XA2_SAMPLE_RATE   48000
#define XA2_CHANNELS      g_xa2_ch
#define XA2_MAX_CHANNELS  6
#define XA2_BUF_SAMPLES   1024   /* ~21ms per submission */
#define XA2_NUM_BUFS      16     /* ring size; how many may queue depends on
                                    * the submission size, see xa2_submit_samples */

static IXAudio2               *g_xa2 = NULL;
static IXAudio2MasteringVoice *g_xa2_master = NULL;
static IXAudio2SourceVoice    *g_xa2_source = NULL;
static int16_t                 g_xa2_bufs[XA2_NUM_BUFS][XA2_BUF_SAMPLES][XA2_MAX_CHANNELS];
static int                     g_xa2_ch = 2;
/* 5.1: centre, LFE and rears, pushed by the mixdown (apu_dsp.c) in the
 * order the front pair is produced, taken again at submission. */
#define SUR_FIFO 16384
static int16_t                 g_sur[SUR_FIFO][4];
static unsigned                g_sur_r, g_sur_w;

int xa2_channels(void)
{
    static int ch = -1;
    if (ch < 0) {
        const char *e = getenv("RECOMP_AUDIO_CHANNELS");
        ch = e && atoi(e) == 6 ? 6 : 2;
    }
    return ch;
}

void xa2_surround_push(const int16_t *s, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        if (g_sur_w - g_sur_r >= SUR_FIFO)
            g_sur_r++;                  /* nobody is taking them: keep the newest */
        memcpy(g_sur[g_sur_w % SUR_FIFO], s + 4 * i, 8);
        g_sur_w++;
    }
}

static void sur_pop(int16_t out[4])
{
    if (g_sur_r != g_sur_w) {
        memcpy(out, g_sur[g_sur_r % SUR_FIFO], 8);
        g_sur_r++;
    } else
        memset(out, 0, 8);
}
static int                     g_xa2_next_buf = 0;
static int                     g_xa2_initialized = 0;
static int                     g_xa2_frames_written = 0;
static int                     g_xa2_underruns, g_xa2_drops, g_xa2_waits, g_xa2_refills;
static int                     g_xa2_min_queued = 1 << 30;

int xa2_init(void)
{
    HRESULT hr;
    int com_initialized;
    WAVEFORMATEX wfx = { 0 };

    if (g_xa2_initialized) return 1;
    g_xa2_ch = xa2_channels();

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        fprintf(stderr, "[XA2] CoInitializeEx failed: 0x%08lX\n", hr);
        return 0;
    }
    com_initialized = SUCCEEDED(hr);

    hr = XAudio2Create(&g_xa2, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (FAILED(hr) || !g_xa2) {
        fprintf(stderr, "[XA2] XAudio2Create failed: 0x%08lX\n", hr);
        goto fail;
    }

    /* 5.1: the device's own channel count; XAudio2 maps (or downmixes) the
     * 5.1 source to it from the channel masks. */
    hr = IXAudio2_CreateMasteringVoice(g_xa2, &g_xa2_master,
        g_xa2_ch == 6 ? XAUDIO2_DEFAULT_CHANNELS : XA2_CHANNELS, XA2_SAMPLE_RATE, 0, NULL, NULL, 0);
    if (FAILED(hr)) {
        fprintf(stderr, "[XA2] CreateMasteringVoice failed: 0x%08lX\n", hr);
        goto fail;
    }

    wfx.wFormatTag      = WAVE_FORMAT_PCM;
    wfx.nChannels       = XA2_CHANNELS;
    wfx.nSamplesPerSec  = XA2_SAMPLE_RATE;
    wfx.wBitsPerSample  = 16;
    wfx.nBlockAlign     = XA2_CHANNELS * 2;
    wfx.nAvgBytesPerSec = XA2_SAMPLE_RATE * wfx.nBlockAlign;

    if (g_xa2_ch == 6) {
        static WAVEFORMATEXTENSIBLE wfe;
        memset(&wfe, 0, sizeof wfe);
        wfe.Format = wfx;
        wfe.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        wfe.Format.cbSize = sizeof wfe - sizeof wfe.Format;
        wfe.Samples.wValidBitsPerSample = 16;
        wfe.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER
                          | SPEAKER_LOW_FREQUENCY | SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT;
        {
            /* KSDATAFORMAT_SUBTYPE_PCM */
            static const GUID pcm = {0x00000001, 0x0000, 0x0010,
                                     {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
            wfe.SubFormat = pcm;
        }
        hr = IXAudio2_CreateSourceVoice(g_xa2, &g_xa2_source,
            &wfe.Format, 0, XAUDIO2_DEFAULT_FREQ_RATIO, NULL, NULL, NULL);
    } else
        hr = IXAudio2_CreateSourceVoice(g_xa2, &g_xa2_source,
            &wfx, 0, XAUDIO2_DEFAULT_FREQ_RATIO, NULL, NULL, NULL);
    if (FAILED(hr)) {
        fprintf(stderr, "[XA2] CreateSourceVoice failed: 0x%08lX\n", hr);
        goto fail;
    }

    hr = IXAudio2SourceVoice_Start(g_xa2_source, 0, XAUDIO2_COMMIT_NOW);
    if (FAILED(hr)) {
        fprintf(stderr, "[XA2] Start failed: 0x%08lX\n", hr);
        goto fail;
    }

    g_xa2_next_buf = 0;
    g_xa2_initialized = 1;
    g_xa2_frames_written = 0;

    fprintf(stderr, "[XA2] XAudio2 initialized (%d Hz %s 16-bit, %d x %d-sample buffers)\n",
            XA2_SAMPLE_RATE, g_xa2_ch == 6 ? "5.1" : "stereo", XA2_NUM_BUFS, XA2_BUF_SAMPLES);
    return 1;

fail:
    xa2_shutdown();
    /* Failed initialization still runs on the COM-initializing thread. */
    if (com_initialized) CoUninitialize();
    return 0;
}

void xa2_shutdown(void)
{
    if (g_xa2_source) {
        IXAudio2SourceVoice_Stop(g_xa2_source, 0, XAUDIO2_COMMIT_NOW);
        IXAudio2SourceVoice_FlushSourceBuffers(g_xa2_source);
        g_xa2_source->lpVtbl->DestroyVoice(g_xa2_source);
        g_xa2_source = NULL;
    }
    if (g_xa2_master) {
        g_xa2_master->lpVtbl->DestroyVoice(g_xa2_master);
        g_xa2_master = NULL;
    }
    if (g_xa2) {
        IXAudio2_Release(g_xa2);
        g_xa2 = NULL;
    }

    if (g_xa2_initialized)
        fprintf(stderr, "[XA2] Shut down (%d frames written)\n", g_xa2_frames_written);
    g_xa2_initialized = 0;
}

int xa2_is_active(void)
{
    return g_xa2_initialized;
}

/* Submit a buffer of mixed samples to XAudio2.
 * Called from APU frame thread. Returns 1 if buffer was submitted. */
/*
 * RECOMP_AUDIO_WAV=<path> -- write what the game plays, 16-bit stereo at the
 * mixer's rate, so the soundtrack can be analysed offline against a scripted
 * ride. The header is patched on exit; a killed process leaves it short by the
 * frames that were not flushed, which the reader can detect and ignore.
 */
#define WAV_RATE 48000
#define WAV_CHANNELS 2

static FILE *g_wav;
static long g_wav_bytes;
static int g_wav_open_tried;

static void wav_put32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
    p[2] = (unsigned char)((v >> 16) & 0xFF);
    p[3] = (unsigned char)((v >> 24) & 0xFF);
}

static void wav_tap_open(void)
{
    unsigned char h[44];
    const char *path;

    if (g_wav_open_tried)
        return;
    g_wav_open_tried = 1;
    path = getenv("RECOMP_AUDIO_WAV");
    if (!path || !*path)
        return;
    g_wav = fopen(path, "wb");
    if (!g_wav) {
        fprintf(stderr, "[WAV] cannot open %s\n", path);
        return;
    }
    memset(h, 0, sizeof h);
    memcpy(h + 0, "RIFF", 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    wav_put32(h + 4, 36);
    wav_put32(h + 16, 16);                 /* fmt chunk size */
    h[20] = 1;                             /* PCM */
    h[22] = WAV_CHANNELS;
    wav_put32(h + 24, WAV_RATE);
    wav_put32(h + 28, WAV_RATE * WAV_CHANNELS * 2);
    h[32] = WAV_CHANNELS * 2;              /* block align */
    h[34] = 16;
    memcpy(h + 36, "data", 4);
    fwrite(h, 1, sizeof h, g_wav);
    fprintf(stderr, "[WAV] capturing the mix to %s\n", path);
}

static void wav_tap_close(void)
{
    unsigned char sz[4];

    if (!g_wav)
        return;
    fflush(g_wav);
    fseek(g_wav, 4, SEEK_SET);
    wav_put32(sz, (uint32_t)(36 + g_wav_bytes));
    fwrite(sz, 1, 4, g_wav);
    fseek(g_wav, 40, SEEK_SET);
    wav_put32(sz, (uint32_t)g_wav_bytes);
    fwrite(sz, 1, 4, g_wav);
    fclose(g_wav);
    g_wav = NULL;
}

static void wav_tap(const int16_t *samples, int num_samples)
{
    size_t n;

    if (!g_wav_open_tried) {
        wav_tap_open();
        if (g_wav)
            atexit(wav_tap_close);
    }
    if (!g_wav || !samples || num_samples <= 0)
        return;
    n = (size_t)num_samples * WAV_CHANNELS;
    g_wav_bytes += (long)fwrite(samples, sizeof(int16_t), n, g_wav);
    if ((g_wav_bytes & 0xFFFF) < (long)num_samples * WAV_CHANNELS)
        fflush(g_wav);
}

int xa2_submit_samples(const int16_t *samples, int num_samples)
{
    XAUDIO2_VOICE_STATE state;
    XAUDIO2_BUFFER xbuf;
    int idx;
    int copy_samples;
    HRESULT hr;

    int16_t sur[XA2_BUF_SAMPLES][4];

    if (!g_xa2_initialized || !g_xa2_source) return 0;
    if (g_xa2_ch == 6) {
        /* Taken now, even if the buffer is dropped below: the FIFO stays in
         * step with the front pair. */
        int k, n = num_samples > XA2_BUF_SAMPLES ? XA2_BUF_SAMPLES : num_samples;
        for (k = 0; k < n; k++)
            sur_pop(sur[k]);
    }

    IXAudio2SourceVoice_GetState(g_xa2_source, &state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
    if (state.BuffersQueued == 0 && g_xa2_frames_written)
        g_xa2_underruns++;
    /* Voice-processor frames (256 samples, 5.3 ms) arrive in step with the
     * card, so the queue keeps whatever depth it had -- in some runs 1-2
     * buffers, where every late frame-thread wake is an underrun (a click).
     * Keep a cushion: start, and restart after running (nearly) dry, with
     * silence up to 8 buffers (43 ms). The full side is handled below. */
    if (num_samples <= 256 && state.BuffersQueued <= 1) {
        static const int16_t silence[256][XA2_MAX_CHANNELS];
        XAUDIO2_BUFFER sb;
        UINT32 k;
        memset(&sb, 0, sizeof(sb));
        sb.AudioBytes = (UINT32)(num_samples * XA2_CHANNELS * sizeof(int16_t));
        sb.pAudioData = (const BYTE *)silence;
        for (k = state.BuffersQueued; k < 7; k++)
            if (FAILED(IXAudio2SourceVoice_SubmitSourceBuffer(g_xa2_source, &sb, NULL)))
                break;
        if (g_xa2_frames_written)
            g_xa2_refills++;
        state.BuffersQueued = k;
    }
    if ((int)state.BuffersQueued < g_xa2_min_queued)
        g_xa2_min_queued = (int)state.BuffersQueued;
    /* ~64 ms of queue either way: 3 buffers of the HLE mixer's 1024 samples,
     * or 12 of the 256-sample frames the emulated APU's voice processor
     * delivers (8 VP frames of 32), which arrive in step with real time and
     * need the slack to ride out a frame thread that is a little late. */
    {
        /* A full queue means this side runs ahead of the sound card (the
         * frame thread is paced by the system clock, which drifts against
         * the card's). The pacing is done by the frame thread before it
         * mixes (apu_core.c throttle, lock released); waiting here held the
         * APU's locks, and the guest's DirectSound writes queued behind them
         * with the guest CPU held -- long sessions crawled at ~20 FPS.
         * Dropping is the last resort. */
        int max_queued = num_samples <= 256 ? 12 : 3;
        if ((int)state.BuffersQueued >= max_queued) { g_xa2_drops++; return 0; }
    }

    idx = g_xa2_next_buf;
    copy_samples = (num_samples > XA2_BUF_SAMPLES) ? XA2_BUF_SAMPLES : num_samples;
    if (g_xa2_ch == 6) {
        int k;
        for (k = 0; k < copy_samples; k++) {
            g_xa2_bufs[idx][k][0] = samples[2 * k];
            g_xa2_bufs[idx][k][1] = samples[2 * k + 1];
            memcpy(&g_xa2_bufs[idx][k][2], sur[k], 8);
        }
    } else
        memcpy(g_xa2_bufs[idx], samples, copy_samples * XA2_CHANNELS * sizeof(int16_t));

    memset(&xbuf, 0, sizeof(xbuf));
    xbuf.AudioBytes = copy_samples * XA2_CHANNELS * sizeof(int16_t);
    xbuf.pAudioData = (const BYTE *)g_xa2_bufs[idx];

    hr = IXAudio2SourceVoice_SubmitSourceBuffer(g_xa2_source, &xbuf, NULL);
    if (FAILED(hr)) return 0;

    g_xa2_next_buf = (idx + 1) % XA2_NUM_BUFS;
    g_xa2_frames_written++;
    return 1;
}

/* Output health since the last call: buffers that found the queue empty
 * (the device ran dry -- an audible gap), buffers dropped because the queue
 * was full, the lowest queue depth seen, submits that waited for room and
 * silence refills of a (nearly) empty queue. */
void xa2_take_stats(int *underruns, int *drops, int *min_queued, int *waits, int *refills)
{
    *waits = g_xa2_waits;
    *refills = g_xa2_refills;
    g_xa2_waits = g_xa2_refills = 0;
    *underruns = g_xa2_underruns;
    *drops = g_xa2_drops;
    *min_queued = g_xa2_min_queued == 1 << 30 ? -1 : g_xa2_min_queued;
    g_xa2_underruns = g_xa2_drops = 0;
    g_xa2_min_queued = 1 << 30;
}

/* Buffers queued for the device now (0 without XAudio2). */
int xa2_queued(void)
{
    XAUDIO2_VOICE_STATE state;
    if (!g_xa2_initialized || !g_xa2_source)
        return 0;
    IXAudio2SourceVoice_GetState(g_xa2_source, &state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
    return (int)state.BuffersQueued;
}

int xa2_get_buffer_size(void)
{
    return XA2_BUF_SAMPLES;
}

#elif defined(__EMSCRIPTEN__) /* -- ring in shared memory, AudioWorklet */

/* The page's AudioWorklet (web/ in the game project) reads this ring straight
 * out of the wasm memory: 48 kHz stereo s16, single producer (the APU frame
 * thread), single consumer (the worklet). Positions count frames and only
 * grow; the worklet plays silence when it catches up. Same queue policy as
 * the other backends, measured in submit-sized buffers. */
#include <emscripten.h>

#define XA2_BUF_SAMPLES   1024
#define RING_FRAMES       16384u            /* power of two, ~340 ms */

typedef struct {
    uint32_t magic;                         /* 'XA2R' */
    uint32_t frames;                        /* ring size in frames */
    volatile uint32_t wr, rd;               /* frame positions */
    volatile uint32_t underruns;            /* bumped by the worklet */
    int16_t data[RING_FRAMES * 2];
} web_ring_t;

static web_ring_t g_ring = { 0x52324158u, RING_FRAMES, 0, 0, 0, {0} };
static int g_web_on, g_web_frames_written, g_web_buf_samples = 256;
static int g_web_underruns, g_web_drops, g_web_refills, g_web_min_queued = 1 << 30;
static uint32_t g_web_seen_underruns;

EMSCRIPTEN_KEEPALIVE void *web_audio_ring(void)
{
    return &g_ring;
}

int xa2_init(void)
{
    g_web_on = 1;
    fprintf(stderr, "[XA2] web audio ring at %p (48000 Hz stereo 16-bit, %u frames)\n",
            (void *)&g_ring, RING_FRAMES);
    return 1;
}

void xa2_shutdown(void) { g_web_on = 0; }
int xa2_is_active(void) { return g_web_on; }

static int web_queued(int buf_samples)
{
    uint32_t n = __atomic_load_n(&g_ring.wr, __ATOMIC_RELAXED)
               - __atomic_load_n(&g_ring.rd, __ATOMIC_ACQUIRE);
    return (int)(n / (uint32_t)buf_samples);
}

static void web_push(const int16_t *s, uint32_t frames)
{
    uint32_t wr = g_ring.wr, i;
    for (i = 0; i < frames; i++) {
        uint32_t at = ((wr + i) & (RING_FRAMES - 1)) * 2;
        g_ring.data[at]     = s ? s[i * 2]     : 0;
        g_ring.data[at + 1] = s ? s[i * 2 + 1] : 0;
    }
    __atomic_store_n(&g_ring.wr, wr + frames, __ATOMIC_RELEASE);
}

int xa2_submit_samples(const int16_t *samples, int num_samples)
{
    int queued, max_queued;
    uint32_t u;

    wav_tap(samples, num_samples);

    if (!g_web_on) return 0;
    if (num_samples > XA2_BUF_SAMPLES) num_samples = XA2_BUF_SAMPLES;
    g_web_buf_samples = num_samples;
    u = __atomic_load_n(&g_ring.underruns, __ATOMIC_RELAXED);
    g_web_underruns += (int)(u - g_web_seen_underruns);
    g_web_seen_underruns = u;
    queued = web_queued(num_samples);
    /* The worklet pulls 128-frame quanta on the audio thread's own clock:
     * keep a deeper cushion than the native queues (8 buffers, ~43 ms). */
    if (num_samples <= 256 && queued <= 1) {
        web_push(NULL, (uint32_t)((8 - queued) * num_samples));
        if (g_web_frames_written)
            g_web_refills++;
        queued = 8;
    }
    if (queued < g_web_min_queued)
        g_web_min_queued = queued;
    max_queued = num_samples <= 256 ? 16 : 4;
    if (queued >= max_queued) { g_web_drops++; return 0; }
    web_push(samples, (uint32_t)num_samples);
    g_web_frames_written++;
    return 1;
}

void xa2_take_stats(int *underruns, int *drops, int *min_queued, int *waits, int *refills)
{
    *waits = 0;
    *refills = g_web_refills;
    *underruns = g_web_underruns;
    *drops = g_web_drops;
    *min_queued = g_web_min_queued == 1 << 30 ? -1 : g_web_min_queued;
    g_web_underruns = g_web_drops = g_web_refills = 0;
    g_web_min_queued = 1 << 30;
}

int xa2_queued(void)
{
    return g_web_on ? web_queued(g_web_buf_samples) : 0;
}

int xa2_get_buffer_size(void)
{
    return XA2_BUF_SAMPLES;
}

#else /* !_WIN32 -- SDL2 audio queue */

int xa2_channels(void) { return 2; }      /* 5.1: Windows (XAudio2) only */
void xa2_surround_push(const int16_t *s, int n) { (void)s; (void)n; }

#include <SDL.h>

#define XA2_SAMPLE_RATE   48000
#define XA2_CHANNELS      2
#define XA2_BUF_SAMPLES   1024

static SDL_AudioDeviceID g_sdl_dev;
static int g_sdl_frames_written;
static int g_sdl_buf_samples = 256;   /* size of the last submit: the unit of "buffers queued" */
static int g_sdl_underruns, g_sdl_drops, g_sdl_refills;
static int g_sdl_min_queued = 1 << 30;

int xa2_init(void)
{
    SDL_AudioSpec want, have;

    if (g_sdl_dev) return 1;
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "[XA2] SDL audio init failed: %s\n", SDL_GetError());
        return 0;
    }
    SDL_zero(want);
    want.freq     = XA2_SAMPLE_RATE;
    want.format   = AUDIO_S16LSB;
    want.channels = XA2_CHANNELS;
    want.samples  = 512;              /* device period, ~11 ms */
    want.callback = NULL;             /* queue mode: SDL_QueueAudio */
    g_sdl_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!g_sdl_dev) {
        fprintf(stderr, "[XA2] SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return 0;
    }
    SDL_PauseAudioDevice(g_sdl_dev, 0);
    g_sdl_frames_written = 0;
    fprintf(stderr, "[XA2] SDL audio initialized (%s, %d Hz stereo 16-bit, period %d)\n",
            SDL_GetCurrentAudioDriver(), have.freq, have.samples);
    return 1;
}

void xa2_shutdown(void)
{
    if (!g_sdl_dev) return;
    SDL_CloseAudioDevice(g_sdl_dev);
    g_sdl_dev = 0;
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    fprintf(stderr, "[XA2] Shut down (%d frames written)\n", g_sdl_frames_written);
}

int xa2_is_active(void)
{
    return g_sdl_dev != 0;
}

static int sdl_queued(int buf_samples)
{
    return (int)(SDL_GetQueuedAudioSize(g_sdl_dev) / (buf_samples * XA2_CHANNELS * sizeof(int16_t)));
}

/* Same queue policy as the XAudio2 path above: a silence cushion of 8
 * buffers after running (nearly) dry, drop when 12 (3 large) are queued. */
int xa2_submit_samples(const int16_t *samples, int num_samples)
{
    int queued, max_queued;

    if (!g_sdl_dev) return 0;
    if (num_samples > XA2_BUF_SAMPLES) num_samples = XA2_BUF_SAMPLES;
    g_sdl_buf_samples = num_samples;
    queued = sdl_queued(num_samples);
    if (queued == 0 && g_sdl_frames_written)
        g_sdl_underruns++;
    if (num_samples <= 256 && queued <= 1) {
        static const int16_t silence[256 * 7][2];
        SDL_QueueAudio(g_sdl_dev, silence,
                       (Uint32)((7 - queued) * num_samples * XA2_CHANNELS * sizeof(int16_t)));
        if (g_sdl_frames_written)
            g_sdl_refills++;
        queued = 7;
    }
    if (queued < g_sdl_min_queued)
        g_sdl_min_queued = queued;
    max_queued = num_samples <= 256 ? 12 : 3;
    if (queued >= max_queued) { g_sdl_drops++; return 0; }
    if (SDL_QueueAudio(g_sdl_dev, samples,
                       (Uint32)(num_samples * XA2_CHANNELS * sizeof(int16_t))) != 0)
        return 0;
    g_sdl_frames_written++;
    return 1;
}

void xa2_take_stats(int *underruns, int *drops, int *min_queued, int *waits, int *refills)
{
    *waits = 0;
    *refills = g_sdl_refills;
    *underruns = g_sdl_underruns;
    *drops = g_sdl_drops;
    *min_queued = g_sdl_min_queued == 1 << 30 ? -1 : g_sdl_min_queued;
    g_sdl_underruns = g_sdl_drops = g_sdl_refills = 0;
    g_sdl_min_queued = 1 << 30;
}

int xa2_queued(void)
{
    return g_sdl_dev ? sdl_queued(g_sdl_buf_samples) : 0;
}

int xa2_get_buffer_size(void)
{
    return XA2_BUF_SAMPLES;
}

#endif /* _WIN32 */
