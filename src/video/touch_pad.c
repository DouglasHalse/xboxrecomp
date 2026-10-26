/* On-screen controls for Android -- see touch_pad.h.
 *
 * A port of the "gamepad" touch overlay from LibertyRecompAndroid
 * (glue/rexglue-sdk-main/gta4-recomp/src/input/gamepad_touch_overlay.cpp): the
 * same geometry formulas, the same colours and alphas, the same hit rules, the
 * same floating sticks and dead zone, and the same rule that the overlay only
 * exists while no real controller is connected.
 *
 * Differences, all of them ours:
 *   - drawn with SDL in window pixels rather than with ImGui in logical ones, so
 *     the pad sits on the glass and not inside the letterboxed frame;
 *   - the pad state reaches the title through the XID report the USB gamepad
 *     path builds (xbox_pad_override), so the guest reads it exactly like a
 *     physical controller;
 *   - fingers arrive as SDL_FINGER* events from the presenter's event loop
 *     rather than being re-read from SDL's touch-device list every frame.
 */
#include "touch_pad.h"

#if defined(__ANDROID__)

#include <android/log.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../usb/usb_gamepad.h"   /* xbox_pad_override */

/* Diagnostics go to logcat directly: this file runs on the presenter thread,
 * whose stderr does not survive the port's log pipe, and "did a controller get
 * detected" is exactly the thing that needs to be observable on a phone. */
#define TLOG(...) __android_log_print(ANDROID_LOG_INFO, "SSX3TOUCH", __VA_ARGS__)

/* ---- the look, from gamepad_touch_overlay.cpp ------------------------- */

#define FILL_R   90
#define FILL_G   90
#define FILL_B   90
#define FILL_A   55

#define HELD_R  200
#define HELD_G  200
#define HELD_B  200
#define HELD_A  105

#define RING_R  215
#define RING_G  215
#define RING_B  215
#define RING_A  105

#define LABEL_R 230
#define LABEL_G 230
#define LABEL_B 230
#define LABEL_A 150

#define KNOB_R  225
#define KNOB_G  225
#define KNOB_B  225
#define KNOB_A  130

/* Face buttons carry their letter colour, as the original does. */
#define FACE_A  { 110, 205,  80, 170 }
#define FACE_B  { 235,  75,  70, 170 }
#define FACE_X  {  75, 145, 240, 170 }
#define FACE_Y  { 245, 205,  50, 170 }

#define SEGMENTS    48
#define MAX_FINGERS 10

/* ---- controls --------------------------------------------------------- */

enum kind { KIND_STICK, KIND_BUTTON, KIND_TRIGGER, KIND_ARROW };

enum {
    BTN_NONE = 0,
    BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT,
    BTN_START, BTN_BACK,
    BTN_A, BTN_B, BTN_X, BTN_Y,
    BTN_LB, BTN_LT, BTN_RB, BTN_RT
};

typedef struct {
    int         kind;
    float       x, y, r;      /* window pixels; a stick's resting place */
    int         button;
    int         index;        /* stick 0/1, trigger 0/1, arrow 0..3 */
    const char *label;
    SDL_Color   label_color;
} control;

#define CONTROL_COUNT 16

static const SDL_Color kLabel = { LABEL_R, LABEL_G, LABEL_B, LABEL_A };
static const float     kDeadZone = 0.12f;

/* ---- state ------------------------------------------------------------ */

static int     s_enabled;         /* the RECOMP_TOUCH_PAD switch */
static int     s_pad_present;     /* a real controller is connected */
static int     s_win_w, s_win_h;

static uint8_t s_digital;
static uint8_t s_analog[8];       /* A B X Y Black White L R */
static int     s_held[CONTROL_COUNT];

static float   s_stick_x[2], s_stick_y[2];
static float   s_stick_ox[2], s_stick_oy[2];
static int     s_stick_owned[2];

typedef struct { SDL_FingerID id; int ctrl; } finger;
static finger  s_fingers[MAX_FINGERS];
static int     s_nfingers;

static SDL_Texture *s_disc;       /* white circle, tinted per use */

/* ---- layout (identical formulas to the original) ---------------------- */

static void build_layout(control *c, float w, float h)
{
    size_t n = 0;
    float lx = 0.12f * w;

    memset(c, 0, sizeof(control) * CONTROL_COUNT);

    /* Sticks first. The right stick has no resting place: it appears where the
     * camera finger lands. */
    c[n++] = (control){ KIND_STICK, lx, 0.72f * h, 0.105f * h, BTN_NONE, 0, "L", kLabel };
    c[n++] = (control){ KIND_STICK, w * 0.75f, 0.60f * h, 0.095f * h, BTN_NONE, 1, "R", kLabel };

    /* D-pad, right of the left stick. */
    {
        float dx = lx + 0.26f * h, dy = 0.86f * h, arm = 0.078f * h, dr = 0.042f * h;
        c[n++] = (control){ KIND_ARROW, dx, dy - arm, dr, BTN_UP, 0, "", kLabel };
        c[n++] = (control){ KIND_ARROW, dx, dy + arm, dr, BTN_DOWN, 1, "", kLabel };
        c[n++] = (control){ KIND_ARROW, dx - arm, dy, dr, BTN_LEFT, 2, "", kLabel };
        c[n++] = (control){ KIND_ARROW, dx + arm, dy, dr, BTN_RIGHT, 3, "", kLabel };
    }

    /* LT over LB above the left stick. The reference also puts L3 here; SSX 3
     * binds nothing to a stick click, so the button would be dead. */
    {
        float br = 0.048f * h;
        c[n++] = (control){ KIND_TRIGGER, lx, 0.355f * h, br, BTN_LT, 0, "LT", kLabel };
        c[n++] = (control){ KIND_BUTTON, lx, 0.475f * h, br, BTN_LB, 0, "LB", kLabel };
    }

    /* A/B/X/Y diamond in the lower right, with the Xbox 360 letter colours. */
    {
        float fx = w - 0.52f * h, fy = 0.80f * h, fs = 0.097f * h, fr = 0.052f * h;
        SDL_Color a = FACE_A, b = FACE_B, x = FACE_X, y = FACE_Y;
        c[n++] = (control){ KIND_BUTTON, fx, fy + fs, fr, BTN_A, 0, "A", a };
        c[n++] = (control){ KIND_BUTTON, fx + fs, fy, fr, BTN_B, 0, "B", b };
        c[n++] = (control){ KIND_BUTTON, fx - fs, fy, fr, BTN_X, 0, "X", x };
        c[n++] = (control){ KIND_BUTTON, fx, fy - fs, fr, BTN_Y, 0, "Y", y };
    }

    /* RT over RB at the right edge (no R3, for the same reason). */
    {
        float br = 0.048f * h;
        float rx = w - 0.17f * h;
        c[n++] = (control){ KIND_TRIGGER, rx, 0.47f * h, br, BTN_RT, 1, "RT", kLabel };
        c[n++] = (control){ KIND_BUTTON, rx, 0.59f * h, br, BTN_RB, 0, "RB", kLabel };
    }

    /* BACK and START in the middle of the top edge. */
    {
        float sr = 0.036f * h;
        c[n++] = (control){ KIND_BUTTON, 0.44f * w, 0.075f * h, sr, BTN_BACK, 0, "BACK", kLabel };
        c[n++] = (control){ KIND_BUTTON, 0.56f * w, 0.075f * h, sr, BTN_START, 0, "START", kLabel };
    }
}

/* ---- drawing ---------------------------------------------------------- */

static SDL_Texture *disc_texture(SDL_Renderer *ren)
{
    const int size = 128;
    SDL_Surface *s;
    int x, y;
    SDL_Texture *t;

    if (s_disc)
        return s_disc;
    s = SDL_CreateRGBSurfaceWithFormat(0, size, size, 32, SDL_PIXELFORMAT_RGBA32);
    if (!s)
        return NULL;
    SDL_LockSurface(s);
    for (y = 0; y < size; y++) {
        Uint32 *row = (Uint32 *)((Uint8 *)s->pixels + y * s->pitch);
        for (x = 0; x < size; x++) {
            float dx = (x + 0.5f) - size * 0.5f, dy = (y + 0.5f) - size * 0.5f;
            float d = sqrtf(dx * dx + dy * dy);
            float edge = size * 0.5f - 0.5f;
            float a = (d <= edge) ? 1.0f : (edge + 1.0f - d);
            if (a < 0.0f) a = 0.0f;
            if (a > 1.0f) a = 1.0f;
            row[x] = SDL_MapRGBA(s->format, 255, 255, 255, (Uint8)(a * 255.0f));
        }
    }
    SDL_UnlockSurface(s);
    t = SDL_CreateTextureFromSurface(ren, s);
    SDL_FreeSurface(s);
    if (t)
        SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    s_disc = t;
    return s_disc;
}

static void fill_circle(SDL_Renderer *ren, SDL_Texture *disc, float cx, float cy,
                        float r, SDL_Color col)
{
    SDL_FRect dst = { cx - r, cy - r, r * 2.0f, r * 2.0f };

    SDL_SetTextureColorMod(disc, col.r, col.g, col.b);
    SDL_SetTextureAlphaMod(disc, col.a);
    SDL_RenderCopyF(ren, disc, NULL, &dst);
}

/* AddCircle(..., 48): a 48-segment outline, 2.5% of the radius thick. */
static void ring_circle(SDL_Renderer *ren, float cx, float cy, float r, SDL_Color col)
{
    int i;
    float prev_x = 0.0f, prev_y = 0.0f;
    float thickness = r * 0.025f < 1.0f ? 1.0f : r * 0.025f;

    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(ren, col.r, col.g, col.b, col.a);
    for (i = 0; i <= SEGMENTS; i++) {
        float a = (float)i * 6.2831853f / (float)SEGMENTS;
        float x = cx + r * cosf(a), y = cy + r * sinf(a);
        if (i) {
            float dx = x - prev_x, dy = y - prev_y;
            float len = sqrtf(dx * dx + dy * dy);
            if (len > 0.0f) {
                float nx = -dy / len * thickness * 0.5f;
                float ny = dx / len * thickness * 0.5f;
                SDL_RenderDrawLineF(ren, prev_x + nx, prev_y + ny, x + nx, y + ny);
                SDL_RenderDrawLineF(ren, prev_x - nx, prev_y - ny, x - nx, y - ny);
            }
        }
        prev_x = x;
        prev_y = y;
    }
}

static void fill_tri(SDL_Renderer *ren, float ax, float ay, float bx, float by,
                     float cx, float cy, SDL_Color col)
{
    SDL_Vertex v[3];

    memset(v, 0, sizeof v);
    v[0].position.x = ax; v[0].position.y = ay;
    v[1].position.x = bx; v[1].position.y = by;
    v[2].position.x = cx; v[2].position.y = cy;
    v[0].color = v[1].color = v[2].color = col;
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    SDL_RenderGeometry(ren, NULL, v, 3, NULL, 0);
}

/* 5x7 glyphs: just the characters the labels use. */
static const unsigned char *glyph(char ch)
{
    static const unsigned char A[7] = { 0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 };
    static const unsigned char B[7] = { 0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E };
    static const unsigned char C[7] = { 0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E };
    static const unsigned char E[7] = { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F };
    static const unsigned char K[7] = { 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11 };
    static const unsigned char L[7] = { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F };
    static const unsigned char R[7] = { 0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11 };
    static const unsigned char S[7] = { 0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E };
    static const unsigned char T[7] = { 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 };
    static const unsigned char X[7] = { 0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11 };
    static const unsigned char Y[7] = { 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04 };
    static const unsigned char N3[7] = { 0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E };

    switch (ch) {
        case 'A': return A;
        case 'B': return B;
        case 'C': return C;
        case 'E': return E;
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

/* One letter fills most of the circle; longer labels are fitted to its width.
 * The original picks a font size (0.95r, or at most 0.62r with a width fit);
 * our glyphs have a cap height of 7 units, so 0.72 of that size is the
 * comparable scale. */
static float label_scale(const char *text, float r)
{
    size_t len = strlen(text);
    float size, scale;

    if (len == 1) {
        size = r * 0.95f;
    } else {
        float fit = r * 1.35f / ((float)len * 0.58f);
        size = r * 0.62f < fit ? r * 0.62f : fit;
    }
    scale = size * 0.72f / 7.0f;
    if (len > 1 && 6.0f * (float)len * scale > 1.45f * r)
        scale = 1.45f * r / (6.0f * (float)len);
    return scale;
}

static void draw_label(SDL_Renderer *ren, const char *text, float cx, float cy,
                       float r, SDL_Color col)
{
    float scale = label_scale(text, r);
    float w, x0, y0;
    size_t i;
    int row, col_i;

    if (scale <= 0.0f)
        return;
    w = (6.0f * (float)strlen(text) - 1.0f) * scale;
    x0 = cx - w * 0.5f;
    y0 = cy - 3.5f * scale;
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(ren, col.r, col.g, col.b, col.a);
    for (i = 0; text[i]; i++) {
        const unsigned char *g = glyph(text[i]);
        if (!g)
            continue;
        for (row = 0; row < 7; row++) {
            for (col_i = 0; col_i < 5; col_i++) {
                if (g[row] & (0x10u >> col_i)) {
                    SDL_FRect px = { x0 + ((float)i * 6.0f + (float)col_i) * scale,
                                     y0 + (float)row * scale,
                                     scale + 0.4f, scale + 0.4f };
                    SDL_RenderFillRectF(ren, &px);
                }
            }
        }
    }
}

static void draw_control(SDL_Renderer *ren, SDL_Texture *disc, const control *c, int held)
{
    SDL_Color fill = held ? (SDL_Color){ HELD_R, HELD_G, HELD_B, HELD_A }
                          : (SDL_Color){ FILL_R, FILL_G, FILL_B, FILL_A };
    SDL_Color ring = { RING_R, RING_G, RING_B, RING_A };

    if (c->r <= 0.0f)
        return;

    if (c->kind == KIND_STICK) {
        float cx = held ? s_stick_ox[c->index] : c->x;
        float cy = held ? s_stick_oy[c->index] : c->y;
        float kx, ky;

        /* The camera stick shows only while it is being used. */
        if (c->index == 1 && !held)
            return;
        fill_circle(ren, disc, cx, cy, c->r, fill);
        ring_circle(ren, cx, cy, c->r, ring);
        kx = cx + s_stick_x[c->index] * c->r * 0.55f;
        ky = cy + s_stick_y[c->index] * c->r * 0.55f;
        fill_circle(ren, disc, kx, ky, c->r * 0.34f,
                    (SDL_Color){ KNOB_R, KNOB_G, KNOB_B, KNOB_A });
        return;
    }

    fill_circle(ren, disc, c->x, c->y, c->r, fill);
    ring_circle(ren, c->x, c->y, c->r, ring);

    if (c->kind == KIND_ARROW) {
        float t = c->r * 0.42f;
        float ax, ay, bx, by, cx2, cy2;

        switch (c->index) {
            case 0: ax = c->x; ay = c->y - t; bx = c->x - t; by = c->y + t * 0.6f;
                    cx2 = c->x + t; cy2 = c->y + t * 0.6f; break;
            case 1: ax = c->x; ay = c->y + t; bx = c->x + t; by = c->y - t * 0.6f;
                    cx2 = c->x - t; cy2 = c->y - t * 0.6f; break;
            case 2: ax = c->x - t; ay = c->y; bx = c->x + t * 0.6f; by = c->y + t;
                    cx2 = c->x + t * 0.6f; cy2 = c->y - t; break;
            default: ax = c->x + t; ay = c->y; bx = c->x - t * 0.6f; by = c->y - t;
                    cx2 = c->x - t * 0.6f; cy2 = c->y + t; break;
        }
        fill_tri(ren, ax, ay, bx, by, cx2, cy2, kLabel);
        return;
    }

    if (c->label && c->label[0])
        draw_label(ren, c->label, c->x, c->y, c->r, c->label_color);
}

/* ---- pad state -------------------------------------------------------- */

static int pad_present(void)
{
    int n, i;

    if (getenv("RECOMP_TOUCH_FORCE_HIDE"))
        return 1;   /* diagnostic: prove the hide path without unplugging a pad */

    /* Android SDL2 also reports the accelerometer as a joystick, and it is not
     * a controller: a controller has buttons, an accelerometer has none, so
     * that is the test -- it also catches a pad SDL has no mapping for, which
     * SDL_IsGameController alone would miss. */
    n = SDL_NumJoysticks();
    for (i = 0; i < n; i++) {
        SDL_Joystick *j;
        int buttons;

        if (SDL_IsGameController(i))
            return 1;
        j = SDL_JoystickOpen(i);
        if (!j)
            continue;
        buttons = SDL_JoystickNumButtons(j);
        SDL_JoystickClose(j);
        if (buttons >= 4)
            return 1;
    }
    return 0;
}

static void set_button(int button, int on)
{
    static const struct { int button; uint8_t bit; int analog; } map[] = {
        { BTN_UP,    0x01, -1 }, { BTN_DOWN,  0x02, -1 },
        { BTN_LEFT,  0x04, -1 }, { BTN_RIGHT, 0x08, -1 },
        { BTN_START, 0x10, -1 }, { BTN_BACK,  0x20, -1 },
        { BTN_A,     0x00,  0 }, { BTN_B,     0x00,  1 },
        { BTN_X,     0x00,  2 }, { BTN_Y,     0x00,  3 },
        /* The original Xbox pad has analog L/R instead of LB/RB: a bumper
         * sends the same trigger, fully pressed. */
        { BTN_LB,    0x00,  6 }, { BTN_LT,    0x00,  6 },
        { BTN_RB,    0x00,  7 }, { BTN_RT,    0x00,  7 },
    };
    size_t i;

    for (i = 0; i < sizeof map / sizeof map[0]; i++) {
        if (map[i].button != button)
            continue;
        if (map[i].bit)
            s_digital = (uint8_t)(on ? (s_digital | map[i].bit)
                                     : (s_digital & ~map[i].bit));
        else
            s_analog[map[i].analog] = (uint8_t)(on ? 255 : 0);
        return;
    }
}

static void release_all(void)
{
    s_digital = 0;
    memset(s_analog, 0, sizeof s_analog);
    memset(s_held, 0, sizeof s_held);
    s_nfingers = 0;
    s_stick_x[0] = s_stick_x[1] = s_stick_y[0] = s_stick_y[1] = 0.0f;
    s_stick_owned[0] = s_stick_owned[1] = 0;
}

static void touch_report(uint8_t *out)
{
    int i;

    if (!s_enabled || s_pad_present)
        return;
    out[2] |= s_digital;
    for (i = 0; i < 8; i++)
        if (s_analog[i] > out[4 + i])
            out[4 + i] = s_analog[i];

    for (i = 0; i < 2; i++) {
        float x = s_stick_x[i], y = s_stick_y[i];
        float len = sqrtf(x * x + y * y);
        int16_t vx = 0, vy = 0;

        /* Radial dead zone, then rescale so the zone's edge is zero and the rim
         * is full: the original's mapping, with screen Y inverted. */
        if (len >= kDeadZone && len > 0.0f) {
            float t = (len - kDeadZone) / (1.0f - kDeadZone);
            float scale;
            if (t > 1.0f) t = 1.0f;
            scale = t / len;
            x *= scale;
            y *= scale;
            if (x > 1.0f) x = 1.0f;
            if (x < -1.0f) x = -1.0f;
            if (y > 1.0f) y = 1.0f;
            if (y < -1.0f) y = -1.0f;
            vx = (int16_t)(x * 32767.0f);
            vy = (int16_t)(-y * 32767.0f);
        }
        if (getenv("RECOMP_TOUCH_TRACE")) {
            /* Only a change worth seeing: what the guest is actually fed when
             * someone presses a button should stay 0,0. */
            static int16_t last_x[2], last_y[2];
            long dx = (long)vx - last_x[i], dy = (long)vy - last_y[i];
            if (dx > 300 || dx < -300 || dy > 300 || dy < -300) {
                TLOG("stick %d -> %d,%d", i, (int)vx, (int)vy);
                last_x[i] = vx;
                last_y[i] = vy;
            }
        }
        if (!vx && !vy)
            continue;
        memcpy(&out[12 + i * 4], &vx, sizeof vx);
        memcpy(&out[14 + i * 4], &vy, sizeof vy);
    }
}

/* ---- touch handling --------------------------------------------------- */

/* The button under a finger, or the stick of that half of the screen. */
static int hit_test(const control *c, float px, float py, float w)
{
    int best = -1, i;
    float best_distance = 1e30f;

    for (i = 0; i < CONTROL_COUNT; i++) {
        float capture, distance;
        if (c[i].r <= 0.0f || c[i].kind == KIND_STICK)
            continue;
        capture = c[i].r * 1.25f;
        distance = sqrtf((px - c[i].x) * (px - c[i].x) + (py - c[i].y) * (py - c[i].y));
        if (distance <= capture && distance / capture < best_distance) {
            best_distance = distance / capture;
            best = i;
        }
    }
    if (best >= 0)
        return best;
    return px < w * 0.5f ? 0 : 1;   /* controls 0 and 1 are the sticks */
}

static int finger_slot(SDL_FingerID id)
{
    int i;

    for (i = 0; i < s_nfingers; i++)
        if (s_fingers[i].id == id)
            return i;
    return -1;
}

static void track_controllers(void)
{
    /* A controller plugged or unplugged mid-game: the overlay follows. */
    int present = pad_present();

    if (present == s_pad_present)
        return;
    s_pad_present = present;
    if (present)
        release_all();
    TLOG("controller %s: on-screen pad %s", present ? "connected" : "removed",
         present ? "hidden" : "shown");
}

/* ---- public ----------------------------------------------------------- */

void touch_pad_init(void)
{
    const char *env = getenv("RECOMP_TOUCH_PAD");

    if (env && (env[0] == '0' || env[0] == 'n' || env[0] == 'N'))
        return;
    /* The accelerometer must not count as a controller: on a phone that has one
     * it would hide the pad forever. */
    SDL_SetHint(SDL_HINT_ACCELEROMETER_AS_JOYSTICK, "0");
    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0)
        TLOG("SDL joystick init failed: %s", SDL_GetError());
    s_pad_present = pad_present();
    s_enabled = 1;
    xbox_pad_override = touch_report;
    {
        int n = SDL_NumJoysticks(), i;

        for (i = 0; i < n; i++) {
            SDL_Joystick *j = SDL_JoystickOpen(i);
            int buttons = j ? SDL_JoystickNumButtons(j) : -1;

            TLOG("joystick %d: \"%s\", %d buttons, game controller: %d", i,
                 SDL_JoystickNameForIndex(i), buttons, SDL_IsGameController(i));
            if (j)
                SDL_JoystickClose(j);
        }
        TLOG("on-screen controls ready (%d joystick(s), controller %s)", n,
             s_pad_present ? "connected: pad hidden" : "absent: pad shown");
    }
}

int touch_pad_enabled(void)
{
    return s_enabled && !s_pad_present;
}

void touch_pad_event(const SDL_Event *e, SDL_Renderer *ren)
{
    control ctrl[CONTROL_COUNT];
    float px, py;
    int index;

    if (!s_enabled || !ren)
        return;
    if (s_win_w <= 0 || s_win_h <= 0) {
        SDL_GetRendererOutputSize(ren, &s_win_w, &s_win_h);
        if (s_win_w <= 0 || s_win_h <= 0)
            return;
    }

    if (e->type == SDL_JOYDEVICEADDED || e->type == SDL_JOYDEVICEREMOVED) {
        track_controllers();
        return;
    }

    /* Touches only. SDL also synthesises a mouse event per tap, in a different
     * coordinate space (2/3 of the surface on this phone), which made a press
     * on START land in the left half and grab the stick. */
    if (e->type != SDL_FINGERDOWN && e->type != SDL_FINGERMOTION &&
        e->type != SDL_FINGERUP)
        return;

    build_layout(ctrl, (float)s_win_w, (float)s_win_h);

    switch (e->type) {
        case SDL_FINGERDOWN: {
            SDL_FingerID id = e->tfinger.fingerId;

            if (s_pad_present || s_nfingers >= MAX_FINGERS || finger_slot(id) >= 0)
                return;
            px = e->tfinger.x * (float)s_win_w;
            py = e->tfinger.y * (float)s_win_h;
            index = hit_test(ctrl, px, py, (float)s_win_w);
            if (index < 0 || index >= CONTROL_COUNT)
                return;

            if (ctrl[index].kind == KIND_STICK) {
                /* A stick centres under the finger, kept fully on screen; one
                 * finger per stick. */
                int stick = ctrl[index].index;
                if (s_stick_owned[stick])
                    return;
                s_stick_owned[stick] = 1;
                s_stick_ox[stick] = px < ctrl[index].r ? ctrl[index].r
                                    : (px > (float)s_win_w - ctrl[index].r
                                           ? (float)s_win_w - ctrl[index].r : px);
                s_stick_oy[stick] = py < ctrl[index].r ? ctrl[index].r
                                    : (py > (float)s_win_h - ctrl[index].r
                                           ? (float)s_win_h - ctrl[index].r : py);
                s_stick_x[stick] = s_stick_y[stick] = 0.0f;
            } else {
                s_held[index] = 1;
                if (ctrl[index].button != BTN_NONE)
                    set_button(ctrl[index].button, 1);
            }
            if (getenv("RECOMP_TOUCH_TRACE"))
                TLOG("down id=%lld %.0f,%.0f -> %d %s", (long long)id, (double)px,
                     (double)py, index, ctrl[index].label);
            s_fingers[s_nfingers].id = id;
            s_fingers[s_nfingers].ctrl = index;
            s_nfingers++;
            return;
        }
        case SDL_FINGERMOTION: {
            SDL_FingerID id = e->tfinger.fingerId;
            int slot = finger_slot(id);
            int stick;

            if (slot < 0)
                return;
            index = s_fingers[slot].ctrl;
            if (ctrl[index].kind != KIND_STICK)
                return;
            px = e->tfinger.x * (float)s_win_w;
            py = e->tfinger.y * (float)s_win_h;
            stick = ctrl[index].index;
            {
                float x = (px - s_stick_ox[stick]) / (ctrl[index].r * 0.85f);
                float y = (py - s_stick_oy[stick]) / (ctrl[index].r * 0.85f);
                float len = sqrtf(x * x + y * y);
                if (len > 1.0f) {
                    x /= len;
                    y /= len;
                }
                s_stick_x[stick] = x;
                s_stick_y[stick] = y;
            }
            return;
        }
        case SDL_FINGERUP: {
            SDL_FingerID id = e->tfinger.fingerId;
            int slot = finger_slot(id);

            if (slot < 0)
                return;
            index = s_fingers[slot].ctrl;
            s_fingers[slot] = s_fingers[--s_nfingers];
            if (getenv("RECOMP_TOUCH_TRACE"))
                TLOG("up id=%lld -> %d %s", (long long)id, index, ctrl[index].label);
            if (ctrl[index].kind == KIND_STICK) {
                int stick = ctrl[index].index;
                s_stick_owned[stick] = 0;
                s_stick_x[stick] = s_stick_y[stick] = 0.0f;
            } else {
                s_held[index] = 0;
                if (ctrl[index].button != BTN_NONE)
                    set_button(ctrl[index].button, 0);
            }
            return;
        }
        default:
            return;
    }
}

void touch_pad_render(SDL_Renderer *ren)
{
    control ctrl[CONTROL_COUNT];
    SDL_Texture *disc;
    int saved_w = 0, saved_h = 0, i;

    if (!s_enabled || !ren)
        return;

    track_controllers();
    if (s_pad_present)
        return;

    SDL_GetRendererOutputSize(ren, &s_win_w, &s_win_h);
    if (s_win_w <= 0 || s_win_h <= 0)
        return;
    build_layout(ctrl, (float)s_win_w, (float)s_win_h);
    disc = disc_texture(ren);
    if (!disc)
        return;

    /* The frame was just copied through the logical transform; the pad lives in
     * window pixels, so drop that transform for these draws. */
    SDL_RenderGetLogicalSize(ren, &saved_w, &saved_h);
    SDL_RenderSetLogicalSize(ren, 0, 0);
    for (i = 0; i < CONTROL_COUNT; i++)
        draw_control(ren, disc, &ctrl[i], s_held[i]);
    SDL_RenderSetLogicalSize(ren, saved_w, saved_h);
}

#endif /* __ANDROID__ */
