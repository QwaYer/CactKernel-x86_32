#include "ps_2_mouse.h"
#include "mouse.h"
#include "kernel.h"

static uint8_t mouse_cycle  = 0;
static int8_t  mouse_byte[3];

static void ps2_wait(uint8_t type) {
    uint32_t timeout = 100000;
    if (type == 0) {
        while (timeout--) if (port_byte_in(0x64) & 1) return;
    } else {
        while (timeout--) if (!(port_byte_in(0x64) & 2)) return;
    }
}

static void ps2_write(uint8_t val) {
    ps2_wait(1); port_byte_out(0x64, 0xD4);
    ps2_wait(1); port_byte_out(0x60, val);
}

static uint8_t ps2_read(void) {
    ps2_wait(0);
    return port_byte_in(0x60);
}

void ps2_mouse_init(void) {
    kprint("[MOUSE] enabling auxiliary PS/2 device (cmd 0xA8)\n");
    ps2_wait(1); port_byte_out(0x64, 0xA8);

    kprint("[MOUSE] enabling IRQ12 in controller config\n");
    ps2_wait(1); port_byte_out(0x64, 0x20);
    ps2_wait(0);
    uint8_t status = port_byte_in(0x60) | 2;
    ps2_wait(1); port_byte_out(0x64, 0x60);
    ps2_wait(1); port_byte_out(0x60, status);

    kprint("[MOUSE] sending 0xF6 (set defaults)  0xF4 (enable data reporting)\n");
    ps2_write(0xF6); ps2_read();
    ps2_write(0xF4); ps2_read();
    kprint("[MOUSE] 3-byte packet mode: btn(1B) dx(1B) dy(1B)\n");
    klog(LOG_OK, "PS/2 mouse ready — IRQ12 active");
}

void ps2_mouse_handler(void) {
    if (!(port_byte_in(0x64) & 0x20)) {
        port_byte_in(0x60);
        return;
    }

    switch (mouse_cycle) {
        case 0:
            mouse_byte[0] = port_byte_in(0x60);
            if (mouse_byte[0] & 0x08) mouse_cycle++;
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

            mouse_post_move(dx, -dy, mouse_byte[0] & 0x07, 0);
            break;
    }
} 