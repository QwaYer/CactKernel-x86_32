#include "mouse.h"
#include "fb.h"

int mouse_x       = 0;
int mouse_y       = 0;
int mouse_buttons = 0;

void mouse_post_move(int x, int y, int buttons, int absolute) {
    uint32_t w = fb_get_width();
    uint32_t h = fb_get_height();

    if (absolute) {
        mouse_x = x;
        mouse_y = y;
    } else {
        mouse_x += x;
        mouse_y += y;
    }

    if (mouse_x < 0)       mouse_x = 0;
    if (mouse_y < 0)       mouse_y = 0;
    if (mouse_x >= (int)w) mouse_x = (int)w - 1;
    if (mouse_y >= (int)h) mouse_y = (int)h - 1;

    mouse_buttons = buttons & 0x07;
}