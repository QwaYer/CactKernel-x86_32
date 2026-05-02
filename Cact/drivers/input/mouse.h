#ifndef MOUSE_H
#define MOUSE_H

#include <stdint.h>

// Current mouse state — updated by PS/2 or USB HID handler
extern int mouse_x;
extern int mouse_y;
extern int mouse_buttons;

// absolute = 0 → relative delta; absolute = 1 → absolute coordinates
void mouse_post_move(int x, int y, int buttons, int absolute);

#endif