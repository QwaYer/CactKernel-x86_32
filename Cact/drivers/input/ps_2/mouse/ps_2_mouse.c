#include "ps_2_mouse.h"
#include "mouse.h"
#include "kernel.h"

// Packet reassembly state
static volatile uint8_t mouse_cycle  = 0;
static volatile int8_t  mouse_byte[3];

// Wait for PS/2 controller status bit: type 0 = read-ready, type 1 = write-ready
static void ps2_wait(uint8_t type) {
    uint32_t timeout = 100000;
    if (type == 0) {
        while (timeout--) if (port_byte_in(0x64) & 1) return;
    } else {
        while (timeout--) if (!(port_byte_in(0x64) & 2)) return;
    }
}

// Send a command byte to the mouse (via port 0xD4)
static void ps2_write(uint8_t val) {
    ps2_wait(1); port_byte_out(0x64, 0xD4);
    ps2_wait(1); port_byte_out(0x60, val);
}

// Read a response byte from the mouse
static uint8_t ps2_read(void) {
    ps2_wait(0);
    return port_byte_in(0x60);
}

// Initialise PS/2 auxiliary device, enable IRQ12, put mouse in 3-byte packet mode
void ps2_mouse_init(void) {
    ps2_wait(1); port_byte_out(0x64, 0xA8);

    ps2_wait(1); port_byte_out(0x64, 0x20);
    ps2_wait(0);
    uint8_t status = port_byte_in(0x60) | 2;   // set IRQ12 enable bit
    ps2_wait(1); port_byte_out(0x64, 0x60);
    ps2_wait(1); port_byte_out(0x60, status);

    ps2_write(0xF6); ps2_read();    // Set Defaults + ACK
    ps2_write(0xF4); ps2_read();    // Enable Data Reporting + ACK
}

// IRQ12 handler: rebuild 3-byte packet, apply sign extension, post mouse event.
// Bit 3 of byte 0 is always 1 — used for synchronisation.
void ps2_mouse_handler(void) {
    // Status register bit 5 must be set — data is from auxiliary device
    if (!(port_byte_in(0x64) & 0x20)) {
        port_byte_in(0x60);   // drain stray byte (from keyboard)
        return;
    }

    switch (mouse_cycle) {
        case 0:
            mouse_byte[0] = port_byte_in(0x60);
            if (mouse_byte[0] & 0x08) mouse_cycle++;   // bit 3 = sync bit
            break;
        case 1:
            mouse_byte[1] = port_byte_in(0x60);
            mouse_cycle++;
            break;
        case 2:
            mouse_byte[2] = port_byte_in(0x60);
            mouse_cycle = 0;

            // Sign-extend 9-bit deltas to 32-bit
            int dx = mouse_byte[1];
            int dy = mouse_byte[2];
            if (mouse_byte[0] & 0x10) dx |= 0xFFFFFF00;   // bit 4 = X sign
            if (mouse_byte[0] & 0x20) dy |= 0xFFFFFF00;   // bit 5 = Y sign

            // Y axis is inverted in PS/2 relative to screen coordinates
            mouse_post_move(dx, -dy, mouse_byte[0] & 0x07, 0);
            break;
    }
}