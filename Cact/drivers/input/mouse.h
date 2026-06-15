#ifndef MOUSE_H
#define MOUSE_H

#include <stdint.h>

// Current mouse state — updated by PS/2 or USB HID handler
extern volatile int mouse_x;
extern volatile int mouse_y;
extern volatile int mouse_buttons;

// Mouse event packet for /dev/mouse
typedef struct {
    int dx;
    int dy;
    int buttons;
    int absolute;   // 0 = relative delta, 1 = absolute coordinates
} mouse_packet_t;

#define MOUSE_BUF_SIZE 64

// absolute = 0 → relative delta; absolute = 1 → absolute coordinates
void mouse_post_move(int x, int y, int buttons, int absolute);

// Read one mouse event from the circular buffer; returns 0 on success, -1 if empty
int  mouse_read_event(mouse_packet_t *pkt);

#endif