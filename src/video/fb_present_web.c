/**
 * Browser counterpart of fb_present_sdl.c: the guest framebuffer for a page.
 *
 * There is no window thread. At each flip the executor converts the guest
 * surface into one of two RGBA buffers (canvas byte order, gamma applied);
 * the page's requestAnimationFrame loop asks web_frame() for the newest one
 * and copies it into its canvas. Keys come from the page through web_key().
 * Same contract otherwise: RECOMP_FB_WINDOW, frame stats, BMP dump,
 * xbox_FramebufferKeyDown.
 */
#include <stdint.h>

#if defined(__EMSCRIPTEN__)
#include <emscripten.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <time.h>
#include <unistd.h>
void xbox_FramebufferApplyGamma(uint32_t *px, size_t n);   /* fb_gamma.c */
extern void *xbox_GuestToHost(uint32_t va);                 /* xbox_memory_layout.c */

static volatile int  s_fb_running;
static uint32_t      s_fb_va, s_fb_pitch, s_fb_width = 640, s_fb_height = 480;
static uint32_t     *s_present[2];
static volatile int  s_present_idx = -1;
static volatile uint32_t s_present_seq;
static volatile unsigned char s_key_down[256];
static char          s_title[48] = "Xbox Recomp";
static volatile int  s_flips, s_frame_draws;
static volatile int  s_client_w, s_client_h;

int xbox_FramebufferClientSize(int *w, int *h)
{
    int cw = __atomic_load_n(&s_client_w, __ATOMIC_RELAXED);
    int ch = __atomic_load_n(&s_client_h, __ATOMIC_RELAXED);
    if (cw <= 0 || ch <= 0)
        return 0;
    *w = cw; *h = ch;
    return 1;
}

void xbox_FramebufferWindowSet(uint32_t fb_va, uint32_t pitch)
{
    const char *pin = getenv("RECOMP_FB_VA");

    s_fb_va = pin ? (uint32_t)strtoul(pin, NULL, 0) : fb_va;
    if (pitch)
        s_fb_pitch = pitch;
}

/* Guest row (X8R8G8B8 or R5G6B5) -> 0xAARRGGBB words. */
static void fb_convert_row(const uint8_t *row, uint32_t *dst, uint32_t bpp)
{
    uint32_t x;

    if (bpp == 4) {
        memcpy(dst, row, (size_t)s_fb_width * 4);
    } else if (bpp == 2) {
        const uint16_t *p = (const uint16_t *)row;
        for (x = 0; x < s_fb_width; x++) {
            uint16_t v = p[x];
            uint32_t r = (uint32_t)((v >> 11) & 0x1F) * 255u / 31u;
            uint32_t g = (uint32_t)((v >>  5) & 0x3F) * 255u / 63u;
            uint32_t b = (uint32_t)( v        & 0x1F) * 255u / 31u;
            dst[x] = (r << 16) | (g << 8) | b;
        }
    } else {
        memset(dst, 0, (size_t)s_fb_width * 4);
    }
}

/* 0xAARRGGBB words -> R,G,B,255 bytes, the canvas's ImageData order. */
static void to_canvas_order(uint32_t *px, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        uint32_t v = px[i];
        px[i] = 0xFF000000u | ((v & 0xFFu) << 16) | (v & 0xFF00u) | ((v >> 16) & 0xFFu);
    }
}

void xbox_FramebufferPublishImage(const uint32_t *px, uint32_t w, uint32_t h)
{
    (void)px; (void)w; (void)h;
}

/* Called by the pushbuffer executor when the title flips. */
void xbox_FramebufferWindowPresent(uint32_t fb_va, uint32_t pitch)
{
    const uint8_t *src;
    int next;
    uint32_t bpp, y;
    size_t n = (size_t)s_fb_width * s_fb_height;

    static int gpu = -1;

    if (!s_fb_running || !fb_va || !pitch)
        return;
    if (gpu < 0)        /* the page's WebGPU renderer shows its frames itself */
        gpu = getenv("RECOMP_WEBGPU") != NULL && !(getenv("RECOMP_GPU") && *getenv("RECOMP_GPU") == 's');
    if (gpu || getenv("RECOMP_FB_VA"))
        return;
    next = (s_present_idx == 0) ? 1 : 0;
    if (!s_present[next]) {
        s_present[next] = (uint32_t *)calloc(n, 4);
        if (!s_present[next])
            return;
    }
    bpp = pitch / s_fb_width;
    src = (const uint8_t *)xbox_GuestToHost(fb_va);
    for (y = 0; y < s_fb_height; y++)
        fb_convert_row(src + (size_t)y * pitch,
                       s_present[next] + (size_t)y * s_fb_width, bpp);
    xbox_FramebufferApplyGamma(s_present[next], n);
    to_canvas_order(s_present[next], n);
    __atomic_store_n(&s_present_idx, next, __ATOMIC_RELEASE);
    __atomic_fetch_add(&s_present_seq, 1, __ATOMIC_RELEASE);
}

/* For the page, once per animation frame. Out: info[0..4] = width, height,
 * frame sequence number, flips so far, draws in the last frame. In:
 * info[6], info[7] = the canvas's display size. Returns the newest frame
 * (RGBA bytes), or 0 before the first flip. */
EMSCRIPTEN_KEEPALIVE uint32_t *web_frame(int32_t *info)
{
    int idx = __atomic_load_n(&s_present_idx, __ATOMIC_ACQUIRE);

    info[0] = (int32_t)s_fb_width;
    info[1] = (int32_t)s_fb_height;
    info[2] = (int32_t)__atomic_load_n(&s_present_seq, __ATOMIC_ACQUIRE);
    info[3] = s_flips;
    info[4] = s_frame_draws;
    if (info[6] > 0 && info[7] > 0) {
        __atomic_store_n(&s_client_w, info[6], __ATOMIC_RELAXED);
        __atomic_store_n(&s_client_h, info[7], __ATOMIC_RELAXED);
    }
    return idx >= 0 ? s_present[idx] : 0;
}

void xbox_FramebufferWindowSetTitle(const uint16_t *name, int max_chars)
{
    int i;

    for (i = 0; i < max_chars && i < 47 && name[i]; i++)
        s_title[i] = (char)name[i];
    if (i)
        s_title[i] = 0;
}

static double now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

/* RECOMP_FRAME_STATS=1: [FRAMES] line once a second, as on the other hosts. */
void xbox_FramebufferWindowFrameStats(uint32_t draws)
{
    static int on = -1;
    static double last, window, dt[2048];
    static int n;
    double t;

    __atomic_fetch_add(&s_flips, 1, __ATOMIC_RELAXED);
    s_frame_draws = (int)draws;
    if (on < 0) {
        const char *e = getenv("RECOMP_FRAME_STATS");
        on = e && *e && *e != '0';
    }
    if (!on)
        return;
    t = now_ms();
    if (last && n < (int)(sizeof dt / sizeof dt[0]))
        dt[n++] = t - last;
    last = t;
    if (!window)
        window = t;
    if (t - window >= 1000.0 && n) {
        double secs = (t - window) / 1000.0;
        qsort(dt, (size_t)n, sizeof dt[0], cmp_double);
        fprintf(stderr, "[FRAMES] n %d fps %.1f p50 %.1f p99 %.1f max %.1f ms draws %u\n",
                n, n / secs, dt[n / 2],
                dt[(n * 99) / 100 < n ? (n * 99) / 100 : n - 1], dt[n - 1], draws);
        n = 0;
        window = t;
    }
}

int xbox_FramebufferKeyDown(int vk)
{
    if ((unsigned)vk > 255)
        return 0;
    return s_key_down[vk] != 0;
}

/* Called from the page with a Windows virtual-key code (web/ maps
 * KeyboardEvent.code); vk < 0 releases every key (focus lost). */
EMSCRIPTEN_KEEPALIVE void web_key(int vk, int down)
{
    if (vk < 0) {
        memset((void *)s_key_down, 0, sizeof s_key_down);
        return;
    }
    if (vk < 256)
        s_key_down[vk] = down != 0;
}

int xbox_FramebufferDumpBmp(const char *path)
{
    FILE *f;
    uint32_t row = ((s_fb_width * 3u) + 3u) & ~3u;
    uint32_t img = row * s_fb_height, total = 54u + img, y, x;
    uint8_t hdr[54], *line;
    int idx = __atomic_load_n(&s_present_idx, __ATOMIC_ACQUIRE);
    const uint32_t *px;

    if (idx < 0 || !s_present[idx])
        return -1;
    px = s_present[idx];
    f = fopen(path, "wb");
    if (!f)
        return -1;
    memset(hdr, 0, sizeof(hdr));
    hdr[0] = 'B'; hdr[1] = 'M';
    memcpy(hdr + 2, &total, 4);
    hdr[10] = 54; hdr[14] = 40;
    memcpy(hdr + 18, &s_fb_width, 4);
    memcpy(hdr + 22, &s_fb_height, 4);
    hdr[26] = 1; hdr[28] = 24;
    memcpy(hdr + 34, &img, 4);
    fwrite(hdr, 1, sizeof(hdr), f);
    line = (uint8_t *)calloc(1, row);
    for (y = 0; y < s_fb_height; y++) {
        const uint32_t *src = px + (size_t)(s_fb_height - 1 - y) * s_fb_width;
        for (x = 0; x < s_fb_width; x++) {          /* canvas order R,G,B,A */
            line[x * 3 + 0] = (uint8_t)((src[x] >> 16) & 0xFF);
            line[x * 3 + 1] = (uint8_t)((src[x] >> 8) & 0xFF);
            line[x * 3 + 2] = (uint8_t)(src[x] & 0xFF);
        }
        fwrite(line, 1, row, f);
    }
    free(line);
    fclose(f);
    fprintf(stderr, "  [FBWIN] wrote %s (%ux%u from 0x%08X)\n",
            path, s_fb_width, s_fb_height, s_fb_va);
    return 0;
}

void xbox_FramebufferWindowStart(void)
{
    if (!getenv("RECOMP_FB_WINDOW"))
        return;
    if (__sync_val_compare_and_swap(&s_fb_running, 0, 1) != 0)
        return;
    fprintf(stderr, "  [FBWIN] presenting to the page (%ux%u)\n",
            s_fb_width, s_fb_height);
}
#endif /* __EMSCRIPTEN__ */
