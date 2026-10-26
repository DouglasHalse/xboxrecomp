/* On-screen controls for Android -- see touch_pad.h.
 *
 * Geometry is normalized: x across the logical frame's width, y down its height,
 * radius as a fraction of its height, so one table serves every resolution and
 * the pad keeps its proportions in the letterbox.
 */
#include "touch_pad.h"

#if defined(__ANDROID__)

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../usb/usb_gamepad.h"   /* xbox_pad_override */

/* ---- controls ------------------------------------------------------- */

enum {
    CTRL_A = 1, CTRL_B, CTRL_X, CTRL_Y,
    CTRL_LB, CTRL_RB, CTRL_LT, CTRL_RT,
    CTRL_START, CTRL_BACK, CTRL_L3, CTRL_R3,
    CTRL_UP, CTRL_DOWN, CTRL_LEFT, CTRL_RIGHT,
    CTRL_STICK,
    CTRL_COUNT
};

typedef struct {
    int         id;
    float       x, y, r;      /* centre and radius, see above */
    const char *label;
    unsigned    colour;       /* label colour, 0xRRGGBBAA */
} ctrl_def;

#define WHITE   0xFFFFFFFFu
#define GREEN   0x8CD33FFFu     /* A */
#define RED     0xE2564CFFu     /* B */
#define BLUE    0x62A8EAFFu     /* X */
#define YELLOW  0xF4CE43FFu     /* Y */

/* Laid out like the LibertyRecomp Android pad: BACK/START centred at the top,
 * the stick bottom-left with the D-pad to its right, the face buttons in the
 * Xbox diamond bottom-right, shoulder cluster above them. */
static const ctrl_def k_ctrls[] = {
    { CTRL_BACK,  0.432f, 0.041f, 0.029f, "BACK",  WHITE  },
    { CTRL_START, 0.549f, 0.041f, 0.029f, "START", WHITE  },

    { CTRL_LT,    0.118f, 0.348f, 0.034f, "LT",    WHITE  },
    { CTRL_LB,    0.118f, 0.463f, 0.034f, "LB",    WHITE  },
    { CTRL_L3,    0.188f, 0.463f, 0.030f, "L3",    WHITE  },

    { CTRL_RT,    0.902f, 0.459f, 0.034f, "RT",    WHITE  },
    { CTRL_RB,    0.902f, 0.589f, 0.034f, "RB",    WHITE  },
    { CTRL_R3,    0.840f, 0.589f, 0.030f, "R3",    WHITE  },

    { CTRL_Y,     0.693f, 0.691f, 0.037f, "Y",     YELLOW },
    { CTRL_X,     0.654f, 0.793f, 0.037f, "X",     BLUE   },
    { CTRL_B,     0.764f, 0.793f, 0.037f, "B",     RED    },
    { CTRL_A,     0.709f, 0.891f, 0.037f, "A",     GREEN  },

    { CTRL_UP,    0.271f, 0.760f, 0.028f, NULL,    WHITE  },
    { CTRL_LEFT,  0.226f, 0.838f, 0.028f, NULL,    WHITE  },
    { CTRL_RIGHT, 0.316f, 0.838f, 0.028f, NULL,    WHITE  },
    { CTRL_DOWN,  0.271f, 0.916f, 0.028f, NULL,    WHITE  },

    { CTRL_STICK, 0.115f, 0.700f, 0.105f, NULL,    WHITE  },
};
#define N_CTRLS ((int)(sizeof k_ctrls / sizeof k_ctrls[0]))

#define STICK_DEADZONE 0.16f
#define MAX_FINGERS    10

/* ---- state ---------------------------------------------------------- */

static int      s_enabled;
static uint8_t  s_digital;
static uint8_t  s_analog[8];      /* A B X Y Black White L R */
static int      s_lx, s_ly;       /* -32767..32767, up positive */
static float    s_knob_x, s_knob_y;   /* -1..1, screen axes */
static int      s_pressed[CTRL_COUNT];

typedef struct { SDL_FingerID id; int ctrl; } finger;
static finger   s_fingers[MAX_FINGERS];
static int      s_nfingers;

/* overlay: static rings and labels, rebuilt when the logical size changes */
static SDL_Texture *s_overlay;
static int          s_overlay_w, s_overlay_h;
static SDL_Texture *s_disc;       /* soft white circle, tinted per use */
static int          s_win_w, s_win_h;   /* window pixels the overlay was built for */

/* ---- pixel helpers -------------------------------------------------- */

static void px(SDL_Surface *s, int x, int y, unsigned r, unsigned g, unsigned b,
               unsigned a)
{
    Uint8 *p;
    unsigned da, sa;

    if (x < 0 || y < 0 || x >= s->w || y >= s->h || a == 0)
        return;
    p = (Uint8 *)s->pixels + (size_t)y * s->pitch + (size_t)x * 4;
    /* source-over, BGRA byte order on little endian ARGB8888 surfaces */
    sa = a;
    da = p[3];
    p[0] = (Uint8)((b * sa + p[0] * da * (255 - sa) / 255) / 255);
    p[1] = (Uint8)((g * sa + p[1] * da * (255 - sa) / 255) / 255);
    p[2] = (Uint8)((r * sa + p[2] * da * (255 - sa) / 255) / 255);
    p[3] = (Uint8)(sa + da * (255 - sa) / 255);
}

/* Filled circle with an optional ring, one pixel of analytic edge softness. */
static void disc(SDL_Surface *s, float cx, float cy, float rad, unsigned rgba,
                 float ring_w, unsigned ring_rgba)
{
    unsigned char r = (unsigned char)(rgba >> 24), g = (unsigned char)(rgba >> 16),
                  b = (unsigned char)(rgba >> 8), a = (unsigned char)rgba;
    unsigned char rr = (unsigned char)(ring_rgba >> 24), rg = (unsigned char)(ring_rgba >> 16),
                  rb = (unsigned char)(ring_rgba >> 8), ra = (unsigned char)ring_rgba;
    int x0 = (int)floorf(cx - rad - ring_w - 1), x1 = (int)ceilf(cx + rad + ring_w + 1);
    int y0 = (int)floorf(cy - rad - ring_w - 1), y1 = (int)ceilf(cy + rad + ring_w + 1);
    int x, y;

    for (y = y0; y <= y1; y++) {
        for (x = x0; x <= x1; x++) {
            float dx = (float)x + 0.5f - cx, dy = (float)y + 0.5f - cy;
            float d = sqrtf(dx * dx + dy * dy);
            if (ring_w > 0.0f) {
                float edge = 0.5f * (rad + rad + ring_w) ; /* mid-ring */
                float half = 0.5f * ring_w;
                float cover = half + 0.5f - fabsf(d - edge);
                if (cover > 0.0f) {
                    if (cover > 1.0f) cover = 1.0f;
                    px(s, x, y, rr, rg, rb, (unsigned)(ra * cover));
                }
            }
            if (a) {
                float cover = rad + 0.5f - d;
                if (cover > 0.0f) {
                    if (cover > 1.0f) cover = 1.0f;
                    px(s, x, y, r, g, b, (unsigned)(a * cover));
                }
            }
        }
    }
}

/* Small triangle, used for the D-pad. dir: 0 up, 1 down, 2 left, 3 right. */
static void tri(SDL_Surface *c, float cx, float cy, float size, int dir, unsigned rgba)
{
    float ax, ay, bx, by, dx2, dy2;
    int x, y, x0, x1, y0, y1;
    unsigned char r = (unsigned char)(rgba >> 24), g = (unsigned char)(rgba >> 16),
                  b = (unsigned char)(rgba >> 8), a = (unsigned char)rgba;

    ax = cx; ay = cy;
    switch (dir) {
        case 0: ax = cx; ay = cy - size; bx = cx - size; by = cy + size * 0.75f;
                dx2 = cx + size; dy2 = cy + size * 0.75f; break;
        case 1: ax = cx; ay = cy + size; bx = cx - size; by = cy - size * 0.75f;
                dx2 = cx + size; dy2 = cy - size * 0.75f; break;
        case 2: ax = cx - size; ay = cy; bx = cx + size * 0.75f; by = cy - size;
                dx2 = cx + size * 0.75f; dy2 = cy + size; break;
        default: ax = cx + size; ay = cy; bx = cx - size * 0.75f; by = cy - size;
                dx2 = cx - size * 0.75f; dy2 = cy + size; break;
    }
    x0 = (int)floorf(fminf(ax, fminf(bx, dx2))) - 1;
    x1 = (int)ceilf(fmaxf(ax, fmaxf(bx, dx2))) + 1;
    y0 = (int)floorf(fminf(ay, fminf(by, dy2))) - 1;
    y1 = (int)ceilf(fmaxf(ay, fmaxf(by, dy2))) + 1;
    for (y = y0; y <= y1; y++) {
        for (x = x0; x <= x1; x++) {
            float pxx = (float)x + 0.5f, pyy = (float)y + 0.5f;
            float e0 = (bx - ax) * (pyy - ay) - (by - ay) * (pxx - ax);
            float e1 = (dx2 - bx) * (pyy - by) - (dy2 - by) * (pxx - bx);
            float e2 = (ax - dx2) * (pyy - dy2) - (ay - dy2) * (pxx - dx2);
            float m = fminf(e0, fminf(e1, e2));
            float cover;
            if (m <= -1.0f)
                continue;
            cover = m >= 1.0f ? 1.0f : (m + 1.0f) * 0.5f;
            px(c, x, y, r, g, b, (unsigned)(a * cover));
        }
    }
}

/* ---- tiny 5x7 font (only the glyphs the pad uses) -------------------- */

static const unsigned char *glyph(char ch)
{
    static const unsigned char A[7] = { 0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 };
    static const unsigned char B[7] = { 0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E };
    static const unsigned char C[7] = { 0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E };
    static const unsigned char K[7] = { 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11 };
    static const unsigned char L[7] = { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F };
    static const unsigned char R[7] = { 0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11 };
    static const unsigned char S[7] = { 0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E };
    static const unsigned char T[7] = { 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 };
    static const unsigned char X[7] = { 0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11 };
    static const unsigned char Y[7] = { 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04 };
    static const unsigned char N3[7] = { 0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E };

    switch (ch) {
        case 'A': return A;
        case 'B': return B;
        case 'C': return C;
        case 'K': return K;
        case 'L': return L;
        case 'R': return R;
        case 'S': return S;
        case 'T': return T;
        case 'X': return X;
        case 'Y': return Y;
        case '3': return N3;
        default:  return NULL;
    }
}

/* Largest scale at which `str` fits inside a circle of radius `rad`. */
static float fit_scale(const char *str, float rad)
{
    float len = (float)strlen(str);
    float by_w = (1.55f * rad) / (len * 6.0f - 1.0f);
    float by_h = (1.15f * rad) / 7.0f;
    float sc = by_w < by_h ? by_w : by_h;
    return sc < 0.6f ? 0.6f : sc;
}

/* Draw text centred on (cx, cy). scale is in pixels per font pixel. */
static void text(SDL_Surface *s, float cx, float cy, const char *str, float scale,
                 unsigned rgba)
{
    size_t n = strlen(str), i;
    float w = (float)n * 6.0f * scale - scale;   /* 5 columns + 1 gap */
    float x = cx - w * 0.5f;
    float y = cy - 3.5f * scale;

    for (i = 0; i < n; i++, x += 6.0f * scale) {
        const unsigned char *g = glyph(str[i]);
        int row, col;
        if (!g)
            continue;
        for (row = 0; row < 7; row++) {
            for (col = 0; col < 5; col++) {
                if (g[row] & (1u << (4 - col))) {
                    int px0 = (int)floorf(x + col * scale), py0 = (int)floorf(y + row * scale);
                    int px1 = (int)ceilf(x + (col + 1) * scale), py1 = (int)ceilf(y + (row + 1) * scale);
                    int xx, yy;
                    for (yy = py0; yy < py1; yy++)
                        for (xx = px0; xx < px1; xx++)
                            px(s, xx, yy, (unsigned char)(rgba >> 24),
                               (unsigned char)(rgba >> 16), (unsigned char)(rgba >> 8),
                               (unsigned char)rgba);
                }
            }
        }
    }
}

/* ---- overlay -------------------------------------------------------- */

static SDL_Texture *overlay_texture(SDL_Renderer *ren, int w, int h)
{
    SDL_Surface *s;
    int i;

    if (s_overlay && s_overlay_w == w && s_overlay_h == h)
        return s_overlay;
    if (s_overlay) {
        SDL_DestroyTexture(s_overlay);
        s_overlay = NULL;
    }
    if (w <= 0 || h <= 0)
        return NULL;
    s = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!s)
        return NULL;
    memset(s->pixels, 0, (size_t)s->pitch * s->h);
    for (i = 0; i < N_CTRLS; i++) {
        const ctrl_def *c = &k_ctrls[i];
        float cx = c->x * (float)w, cy = c->y * (float)h, rad = c->r * (float)h;

        if (c->id == CTRL_STICK) {
            disc(s, cx, cy, rad, 0xFFFFFF10u, 2.0f, 0xFFFFFFA0u);
            continue;
        }
        disc(s, cx, cy, rad, 0x00000055u, 1.6f, 0xFFFFFFB0u);
        if (c->label) {
            text(s, cx, cy, c->label, fit_scale(c->label, rad), c->colour);
        } else {
            int dir = c->id == CTRL_UP ? 0 : c->id == CTRL_DOWN ? 1 :
                      c->id == CTRL_LEFT ? 2 : 3;
            tri(s, cx, cy, rad * 0.55f, dir, 0xFFFFFFE0u);
        }
    }
    s_overlay = SDL_CreateTextureFromSurface(ren, s);
    SDL_FreeSurface(s);
    if (s_overlay)
        SDL_SetTextureBlendMode(s_overlay, SDL_BLENDMODE_BLEND);
    s_overlay_w = w;
    s_overlay_h = h;
    return s_overlay;
}

/* A soft white circle, tinted at draw time. */
static SDL_Texture *disc_texture(SDL_Renderer *ren)
{
    SDL_Surface *s;
    const int n = 128;

    if (s_disc)
        return s_disc;
    s = SDL_CreateRGBSurfaceWithFormat(0, n, n, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!s)
        return NULL;
    memset(s->pixels, 0, (size_t)s->pitch * s->h);
    disc(s, n * 0.5f, n * 0.5f, n * 0.5f - 1.0f, 0xFFFFFFFFu, 0.0f, 0);
    s_disc = SDL_CreateTextureFromSurface(ren, s);
    SDL_FreeSurface(s);
    if (s_disc)
        SDL_SetTextureBlendMode(s_disc, SDL_BLENDMODE_BLEND);
    return s_disc;
}

/* ---- report --------------------------------------------------------- */

static void set_button(int id, int on)
{
    static const struct { int id; uint8_t bit; int analog; } map[] = {
        { CTRL_UP,    0x01, -1 }, { CTRL_DOWN,  0x02, -1 },
        { CTRL_LEFT,  0x04, -1 }, { CTRL_RIGHT, 0x08, -1 },
        { CTRL_START, 0x10, -1 }, { CTRL_BACK,  0x20, -1 },
        { CTRL_L3,    0x40, -1 }, { CTRL_R3,    0x80, -1 },
        { CTRL_A,     0x00,  0 }, { CTRL_B,     0x00,  1 },
        { CTRL_X,     0x00,  2 }, { CTRL_Y,     0x00,  3 },
        /* The original Xbox pad has analog L/R instead of LB/RB: a bumper
         * sends the same trigger, fully pressed. */
        { CTRL_LB,    0x00,  6 }, { CTRL_LT,    0x00,  6 },
        { CTRL_RB,    0x00,  7 }, { CTRL_RT,    0x00,  7 },
    };
    size_t i;

    for (i = 0; i < sizeof map / sizeof map[0]; i++) {
        if (map[i].id != id)
            continue;
        if (map[i].bit)
            s_digital = (uint8_t)(on ? (s_digital | map[i].bit)
                                     : (s_digital & ~map[i].bit));
        else
            s_analog[map[i].analog] = (uint8_t)(on ? 255 : 0);
        return;
    }
}

static void touch_report(uint8_t *out)
{
    int i;

    if (!s_enabled)
        return;
    out[2] |= s_digital;
    for (i = 0; i < 8; i++)
        if (s_analog[i] > out[4 + i])
            out[4 + i] = s_analog[i];
    if (s_lx || s_ly) {
        int16_t vx = (int16_t)s_lx, vy = (int16_t)s_ly;
        memcpy(&out[12], &vx, sizeof vx);
        memcpy(&out[14], &vy, sizeof vy);
    }
}

/* ---- touch handling ------------------------------------------------- */

/* Window pixels. The pad lives on the glass, not in the game's letterboxed
 * frame, so it spreads across the whole screen like the reference layout. */
static void window_point(SDL_Renderer *ren, const SDL_Event *e, float *lx, float *ly)
{
    int w = s_win_w, h = s_win_h;

    if (w <= 0 || h <= 0) {
        SDL_GetRendererOutputSize(ren, &w, &h);
        if (w <= 0 || h <= 0) { w = 1; h = 1; }
    }
    switch (e->type) {
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP:
            *lx = (float)e->button.x; *ly = (float)e->button.y; break;
        case SDL_MOUSEMOTION:
            *lx = (float)e->motion.x; *ly = (float)e->motion.y; break;
        default:
            *lx = e->tfinger.x * (float)w;
            *ly = e->tfinger.y * (float)h;
            break;
    }
}

/* Which control is under this point? Sticks and buttons share the table. */
static int hit(float lx, float ly, int w, int h, float *out_dx, float *out_dy)
{
    int i, best = 0;
    float best_d = 1e9f;

    for (i = 0; i < N_CTRLS; i++) {
        const ctrl_def *c = &k_ctrls[i];
        float cx = c->x * (float)w, cy = c->y * (float)h, rad = c->r * (float)h;
        float dx = lx - cx, dy = ly - cy, d = sqrtf(dx * dx + dy * dy);
        /* A third again the ring's radius: a fingertip is wider than the
         * circle it aims at, and a near miss reads as a dead button. */
        if (d <= rad * 1.35f && d < best_d) {
            best_d = d;
            best = c->id;
            if (out_dx) *out_dx = dx / rad;
            if (out_dy) *out_dy = dy / rad;
        }
    }
    return best;
}

static void stick_axis(float dx, float dy)
{
    float m = sqrtf(dx * dx + dy * dy);

    if (m > 1.0f) { dx /= m; dy /= m; m = 1.0f; }
    s_knob_x = dx;
    s_knob_y = dy;
    if (m < STICK_DEADZONE) {
        s_lx = s_ly = 0;
        return;
    }
    /* rescale so the deadzone edge is zero, the rim is full */
    m = (m - STICK_DEADZONE) / (1.0f - STICK_DEADZONE);
    s_lx = (int)(dx / (sqrtf(dx * dx + dy * dy) + 1e-6f) * m * 32767.0f);
    s_ly = (int)(-dy / (sqrtf(dx * dx + dy * dy) + 1e-6f) * m * 32767.0f);
}

static int finger_index(SDL_FingerID id)
{
    int i;
    for (i = 0; i < s_nfingers; i++)
        if (s_fingers[i].id == id)
            return i;
    return -1;
}

void touch_pad_event(const SDL_Event *e, SDL_Renderer *ren)
{
    float lx = 0, ly = 0, dx = 0, dy = 0;
    int lw = 0, lh = 0, idx, ctrl;
    SDL_FingerID id;

    if (!s_enabled || !ren)
        return;
    if (s_win_w <= 0 || s_win_h <= 0) {
        SDL_GetRendererOutputSize(ren, &lw, &lh);
        if (lw <= 0 || lh <= 0)
            return;
        s_win_w = lw;
        s_win_h = lh;
    }
    lw = s_win_w;
    lh = s_win_h;

    if (getenv("RECOMP_TOUCH_TRACE")) {
        fputs("  [TOUCH] event type ", stderr);
        fprintf(stderr, "0x%X win=%dx%d ", (unsigned)e->type, lw, lh);
        if (e->type == SDL_MOUSEMOTION)
            fprintf(stderr, "mouse=%.0f,%.0f", (double)e->motion.x, (double)e->motion.y);
        else
            fprintf(stderr, "finger=%.3f,%.3f", (double)e->tfinger.x, (double)e->tfinger.y);
        fputc('\n', stderr);
    }

    switch (e->type) {
        case SDL_FINGERDOWN:
        case SDL_MOUSEBUTTONDOWN:
            id = (e->type == SDL_MOUSEBUTTONDOWN) ? (SDL_FingerID)1 : e->tfinger.fingerId;
            window_point(ren, e, &lx, &ly);
            ctrl = hit(lx, ly, lw, lh, &dx, &dy);
            if (getenv("RECOMP_TOUCH_TRACE")) {
                fputs("  [TOUCH] down at ", stderr);
                fprintf(stderr, "%.0f,%.0f -> control %d", (double)lx, (double)ly, ctrl);
                fputc('\n', stderr);
            }
            if (!ctrl || s_nfingers >= MAX_FINGERS)
                return;
            s_fingers[s_nfingers].id = id;
            s_fingers[s_nfingers].ctrl = ctrl;
            s_nfingers++;
            if (ctrl == CTRL_STICK) {
                stick_axis(dx, dy);
            } else {
                s_pressed[ctrl]++;
                if (s_pressed[ctrl] == 1)
                    set_button(ctrl, 1);
            }
            break;

        case SDL_FINGERMOTION:
        case SDL_MOUSEMOTION:
            id = (e->type == SDL_MOUSEMOTION) ? (SDL_FingerID)1 : e->tfinger.fingerId;
            idx = finger_index(id);
            if (idx < 0 || s_fingers[idx].ctrl != CTRL_STICK)
                return;
            window_point(ren, e, &lx, &ly);
            {
                const ctrl_def *c = &k_ctrls[N_CTRLS - 1];   /* the stick */
                float cx = c->x * (float)lw, cy = c->y * (float)lh, rad = c->r * (float)lh;
                stick_axis((lx - cx) / rad, (ly - cy) / rad);
            }
            break;

        case SDL_FINGERUP:
        case SDL_MOUSEBUTTONUP:
            id = (e->type == SDL_MOUSEBUTTONUP) ? (SDL_FingerID)1 : e->tfinger.fingerId;
            idx = finger_index(id);
            if (idx < 0)
                return;
            ctrl = s_fingers[idx].ctrl;
            s_fingers[idx] = s_fingers[--s_nfingers];
            if (ctrl == CTRL_STICK) {
                s_lx = s_ly = 0;
                s_knob_x = s_knob_y = 0.0f;
            } else if (s_pressed[ctrl] > 0 && --s_pressed[ctrl] == 0) {
                set_button(ctrl, 0);
            }
            break;
        default:
            break;
    }
}

/* ---- drawing -------------------------------------------------------- */

void touch_pad_render(SDL_Renderer *ren)
{
    SDL_Texture *ov, *dc;
    int lw = 0, lh = 0, i;

    if (!s_enabled || !ren)
        return;
    int saved_w = 0, saved_h = 0;

    SDL_RenderGetLogicalSize(ren, &saved_w, &saved_h);
    SDL_GetRendererOutputSize(ren, &lw, &lh);
    if (lw <= 0 || lh <= 0)
        return;
    s_win_w = lw;
    s_win_h = lh;
    /* The game frame was just copied through the logical transform; the pad is
     * drawn in window pixels, so drop that transform for these draws. */
    SDL_RenderSetLogicalSize(ren, 0, 0);
    ov = overlay_texture(ren, lw, lh);
    if (!ov) {
        SDL_RenderSetLogicalSize(ren, saved_w, saved_h);
        return;
    }
    SDL_RenderCopy(ren, ov, NULL, NULL);

    dc = disc_texture(ren);
    if (!dc) {
        SDL_RenderSetLogicalSize(ren, saved_w, saved_h);
        return;
    }

    for (i = 0; i < N_CTRLS; i++) {
        const ctrl_def *c = &k_ctrls[i];
        float cx = c->x * (float)lw, cy = c->y * (float)lh, rad = c->r * (float)lh;
        SDL_Rect dst;

        if (c->id == CTRL_STICK)
            continue;
        if (s_pressed[c->id] <= 0)
            continue;
        dst.x = (int)(cx - rad); dst.y = (int)(cy - rad);
        dst.w = dst.h = (int)(rad * 2.0f);
        SDL_SetTextureColorMod(dc, 255, 255, 255);
        SDL_SetTextureAlphaMod(dc, 90);
        SDL_RenderCopy(ren, dc, NULL, &dst);
    }

    /* stick: knob wherever the finger has dragged it */
    {
        const ctrl_def *c = &k_ctrls[N_CTRLS - 1];
        float cx = c->x * (float)lw, cy = c->y * (float)lh, rad = c->r * (float)lh;
        float kx = cx + s_knob_x * rad * 0.62f, ky = cy + s_knob_y * rad * 0.62f;
        float kr = rad * 0.42f;
        SDL_Rect dst;
        dst.x = (int)(kx - kr); dst.y = (int)(ky - kr);
        dst.w = dst.h = (int)(kr * 2.0f);
        SDL_SetTextureColorMod(dc, 255, 255, 255);
        SDL_SetTextureAlphaMod(dc, (s_lx || s_ly) ? 190 : 110);
        SDL_RenderCopy(ren, dc, NULL, &dst);
    }
    SDL_RenderSetLogicalSize(ren, saved_w, saved_h);
}

/* ---- init ----------------------------------------------------------- */

int touch_pad_enabled(void)
{
    return s_enabled;
}

void touch_pad_init(void)
{
    const char *env = getenv("RECOMP_TOUCH_PAD");

    if (env && (env[0] == '0' || env[0] == 'n' || env[0] == 'N')) {
        s_enabled = 0;
        return;
    }
    s_enabled = 1;
    s_overlay_w = s_overlay_h = 0;
    xbox_pad_override = touch_report;
    fprintf(stderr, "  [TOUCH] on-screen controls on (RECOMP_TOUCH_PAD=0 hides them)\n");
}

#endif /* __ANDROID__ */
