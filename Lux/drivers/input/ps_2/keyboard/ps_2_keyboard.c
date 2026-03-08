#include "ps_2_keyboard.h"
#include "keyboard.h"
#include "kernel.h"

static unsigned char keymap[128] = {
    0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0,
    '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' '
};

static unsigned char keymap_shift[128] = {
    0,  27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0,
    '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' '
};

static int shift_pressed    = 0;
static int caps_lock_active = 0;

int ps2_keyboard_init(void) {
    while (port_byte_in(0x64) & 0x01)
        port_byte_in(0x60);

    while (port_byte_in(0x64) & 0x02);
    port_byte_out(0x64, 0xAE);

    while (port_byte_in(0x64) & 0x02);
    port_byte_out(0x64, 0x20);
    while (!(port_byte_in(0x64) & 0x01));
    unsigned char config = port_byte_in(0x60);
    config |= 0x01;

    while (port_byte_in(0x64) & 0x02);
    port_byte_out(0x64, 0x60);
    while (port_byte_in(0x64) & 0x02);
    port_byte_out(0x60, config);

    while (port_byte_in(0x64) & 0x01)
        port_byte_in(0x60);

    return 0;
}

void ps2_keyboard_handler(void) {
    unsigned char scancode = port_byte_in(0x60);

    if      (scancode == 0x2A || scancode == 0x36) { shift_pressed = 1;             return; }
    else if (scancode == 0xAA || scancode == 0xB6) { shift_pressed = 0;             return; }
    else if (scancode == 0x3A) { caps_lock_active = !caps_lock_active;              return; }

    if (scancode & 0x80) return;

    char c = keymap[scancode];
    if (!c) return;

    if (c >= 'a' && c <= 'z') {
        if (shift_pressed != caps_lock_active)
            c = keymap_shift[scancode];
    } else if (shift_pressed) {
        c = keymap_shift[scancode];
    }

    keyboard_post_key(c);
}