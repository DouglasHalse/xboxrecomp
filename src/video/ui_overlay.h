/*
 * ui_overlay.h -- a port's own screens, over the presented image.
 *
 * The presenters own the window; a port that wants an options screen in the
 * game installs these hooks and draws into the frame it is handed. Called on
 * the thread that owns the window, once per presented frame (Windows: the
 * GDI/D3D11 window loop; SDL: the presenter loop).
 *
 * All of them are optional: NULL means "the port has no screens".
 */
#ifndef XBOX_UI_OVERLAY_H
#define XBOX_UI_OVERLAY_H

#include <stdint.h>

/* Draw over a finished frame, in place. `px` is 0x00RRGGBB, `w`x`h` the
 * frame's own pixels (not window pixels): that is what makes an overlay look
 * identical at every internal resolution. */
extern void (*volatile xbox_ui_draw)(uint32_t *px, uint32_t w, uint32_t h);

/* A window key, VK code (both presenters map to VK). Returns 1 when the port
 * took it -- the key then neither reaches the title nor moves the pad. */
extern int (*volatile xbox_ui_key)(int vk, int down);

/* A touch in window pixels. phase: 0 down, 1 move, 2 up. Returns 1 when the
 * port took it. */
extern int (*volatile xbox_ui_touch)(float x, float y, int phase);

/* 1 while the port's screen is up and the title must not see input. */
extern int (*volatile xbox_ui_blocks_input)(void);

/* The finished pad report, before the title reads it: a port screen inspects
 * the buttons and returns 0 to swallow them (the report is neutralised, the
 * device and its stick values stay). Called on the input thread. */
extern int (*volatile xbox_ui_pad)(uint8_t *report, int len);

/* The title's own pad state, on the path the title actually polls: `buttons`
 * points at XINPUT_GAMEPAD.wButtons, `analog` at its eight pressure bytes.
 * Return 0 to swallow the press (both are zeroed by the caller). */
extern int (*volatile xbox_ui_buttons)(unsigned short *buttons, unsigned char *analog);

/* The presenter's own frame counter (F3 on a desktop window), for a port that
 * offers it as a setting. A no-op where the presenter has none. */
void xbox_FramebufferSetFpsOverlay(int on);

/* The above with the NULL checks done (xbox_ui_*.c). */
void xbox_UiDraw(uint32_t *px, uint32_t w, uint32_t h);
int  xbox_UiKey(int vk, int down);
int  xbox_UiTouch(float x, float y, int phase);
int  xbox_UiBlocksInput(void);
int  xbox_UiPad(uint8_t *report, int len);

#endif /* XBOX_UI_OVERLAY_H */
