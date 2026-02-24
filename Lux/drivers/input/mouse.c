#include "mouse.h"
#include "kernel.h"
#include "libc.h"

int mouse_x = 0;
int mouse_y = 0;
int mouse_buttons = 0;

static uint8_t mouse_cycle = 0;
static int8_t mouse_byte[3];

static void mouse_wait(uint8_t a_type) {
    uint32_t _time_out = 100000;
    if (a_type == 0) {
        while (_time_out--) {
            if ((port_byte_in(0x64) & 1) == 1) {
                return;
            }
        }
        return;
    } else {
        while (_time_out--) {
            if ((port_byte_in(0x64) & 2) == 0) {
                return;
            }
        }
        return;
    }
}

static void mouse_write(uint8_t a_write) {
    mouse_wait(1);
    port_byte_out(0x64, 0xD4);
    mouse_wait(1);
    port_byte_out(0x60, a_write);
}

static uint8_t mouse_read() {
    mouse_wait(0);
    return port_byte_in(0x60);
}

void init_mouse() {
    uint8_t _status;

    // Enable the auxiliary mouse device
    mouse_wait(1);
    port_byte_out(0x64, 0xA8);

    // Enable the interrupts
    mouse_wait(1);
    port_byte_out(0x64, 0x20);
    mouse_wait(0);
    _status = (port_byte_in(0x60) | 2);
    mouse_wait(1);
    port_byte_out(0x64, 0x60);
    mouse_wait(1);
    port_byte_out(0x60, _status);

    // Tell the mouse to use default settings
    mouse_write(0xF6);
    mouse_read();  // Acknowledge

    // Enable the mouse
    mouse_write(0xF4);
    mouse_read();  // Acknowledge
}

void mouse_handler() {
    uint8_t status = port_byte_in(0x64);
    if (!(status & 0x20)) {
        port_byte_in(0x60);
        return;
    }

    switch (mouse_cycle) {
        case 0:
            mouse_byte[0] = port_byte_in(0x60);
            if (mouse_byte[0] & 0x08) { 
                mouse_cycle++;
            }
            break;
        case 1:
            mouse_byte[1] = port_byte_in(0x60);
            mouse_cycle++;
            break;
        case 2:
            mouse_byte[2] = port_byte_in(0x60);
            mouse_cycle = 0;

            int dx = mouse_byte[1];
            int dy = mouse_byte[2];

            if (mouse_byte[0] & 0x10) dx |= 0xFFFFFF00;
            if (mouse_byte[0] & 0x20) dy |= 0xFFFFFF00;

            mouse_x += dx;
            mouse_y -= dy; 

            
            if (mouse_x < 0) mouse_x = 0;
            if (mouse_x >= MAX_COLS * 8) mouse_x = MAX_COLS * 8 - 1; 
            if (mouse_y < 0) mouse_y = 0;
            if (mouse_y >= MAX_ROWS * 16) mouse_y = MAX_ROWS * 16 - 1; 

            mouse_buttons = mouse_byte[0] & 0x07;
            break;
    }
}
