#ifndef MOUSE_H
#define MOUSE_H

#include <stdint.h>

extern int mouse_x;
extern int mouse_y;
extern int mouse_buttons;

void mouse_post_move(int x, int y, int buttons, int absolute);

#endif