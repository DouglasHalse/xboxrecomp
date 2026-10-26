/* On-screen controls for Android.
 *
 * The pad is drawn over the presented frame and reports as pad 0 through the
 * same XID report the USB gamepad path builds, so the title sees exactly what a
 * physical controller would send. Layout and look follow the LibertyRecomp
 * Android port's touch gamepad: translucent rings with coloured face labels, a
 * left analog stick, the D-pad beside it, and the shoulder/trigger/stick-click
 * cluster on the right.
 *
 * Only built for Android (see src/video/CMakeLists.txt): the drawing needs the
 * SDL renderer the Android presenter owns.
 */
#ifndef XBOX_TOUCH_PAD_H
#define XBOX_TOUCH_PAD_H

#include <SDL2/SDL.h>

/* Install the pad and hook it into the pad report. RECOMP_TOUCH_PAD=0 turns it
 * off; on Android it is on by default. Safe to call once, from the thread that
 * owns the window. */
void touch_pad_init(void);

/* Feed window input events (call for every event the presenter polls). */
void touch_pad_event(const SDL_Event *e, SDL_Renderer *ren);

/* Draw the pad over the current frame, before the present. */
void touch_pad_render(SDL_Renderer *ren);

/* 1 when the pad is showing. */
int touch_pad_enabled(void);

#endif /* XBOX_TOUCH_PAD_H */
