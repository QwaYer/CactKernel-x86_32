#ifndef MOUSE_H
#define MOUSE_H

#include <stdint.h>

void init_mouse();
void mouse_handler();

extern int mouse_x;
extern int mouse_y;
extern int mouse_buttons;

#endif
