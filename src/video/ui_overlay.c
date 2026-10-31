/* ui_overlay.h: the port's hooks, defined once for whichever presenter is
 * built (fb_present.c on Windows, fb_present_sdl.c elsewhere). */
#include "ui_overlay.h"

void (*volatile xbox_ui_draw)(uint32_t *px, uint32_t w, uint32_t h);
int  (*volatile xbox_ui_key)(int vk, int down);
int  (*volatile xbox_ui_touch)(float x, float y, int phase);
int  (*volatile xbox_ui_blocks_input)(void);

void xbox_UiDraw(uint32_t *px, uint32_t w, uint32_t h)
{
    if (xbox_ui_draw && px && w && h)
        xbox_ui_draw(px, w, h);
}

int xbox_UiKey(int vk, int down)
{
    return xbox_ui_key ? xbox_ui_key(vk, down) : 0;
}

int xbox_UiTouch(float x, float y, int phase)
{
    return xbox_ui_touch ? xbox_ui_touch(x, y, phase) : 0;
}

int xbox_UiBlocksInput(void)
{
    return xbox_ui_blocks_input ? xbox_ui_blocks_input() : 0;
}
