#include "ps_2_keyboard.h"
#include "keyboard.h"
#include "kernel.h"
#include "task.h"

// PS/2 Set 1 scancode → ASCII (unmodified)
static unsigned char keymap[128] = {
    0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0,
    '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' '
};

// PS/2 Set 1 scancode → ASCII (Shift-modified)
static unsigned char keymap_shift[128] = {
    0,  27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0,
    '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' '
};

// Modifier state — toggled on press/release
static int shift_pressed    = 0;
static int caps_lock_active = 0;
static int ctrl_pressed     = 0;

// Set 1 scancodes for Ctrl-combo signal generation
#define SCANCODE_C      0x2E   // 'c' — Ctrl-C → SIGINT
#define SCANCODE_BSLASH 0x2B   // '\' — Ctrl-\ → SIGQUIT

// Initialise PS/2 controller: enable first port, unmask IRQ1
int ps2_keyboard_init(void) {
    uint32_t timeout;
    for (timeout = 100000; timeout && (port_byte_in(0x64) & 0x01); timeout--)
        port_byte_in(0x60);

    for (timeout = 100000; timeout && (port_byte_in(0x64) & 0x02); timeout--);
    if (!timeout) return -1;
    port_byte_out(0x64, 0xAE);

    for (timeout = 100000; timeout && (port_byte_in(0x64) & 0x02); timeout--);
    if (!timeout) return -1;
    port_byte_out(0x64, 0x20);
    for (timeout = 100000; timeout && !(port_byte_in(0x64) & 0x01); timeout--);
    if (!timeout) return -1;
    unsigned char config = port_byte_in(0x60);
    config |= 0x01;  // enable IRQ1

    for (timeout = 100000; timeout && (port_byte_in(0x64) & 0x02); timeout--);
    if (!timeout) return -1;
    port_byte_out(0x64, 0x60);
    for (timeout = 100000; timeout && (port_byte_in(0x64) & 0x02); timeout--);
    if (!timeout) return -1;
    port_byte_out(0x60, config);

    for (timeout = 100000; timeout && (port_byte_in(0x64) & 0x01); timeout--)
        port_byte_in(0x60);

    return 0;
}

// IRQ1 handler: decode scancode, update modifiers, post character or signal
void ps2_keyboard_handler(void) {
    unsigned char scancode = port_byte_in(0x60);

    // Modifier press/release tracking
    if      (scancode == 0x2A || scancode == 0x36) { shift_pressed = 1;             return; }
    else if (scancode == 0xAA || scancode == 0xB6) { shift_pressed = 0;             return; }
    else if (scancode == 0x1D)                     { ctrl_pressed  = 1;             return; }
    else if (scancode == 0x9D)                     { ctrl_pressed  = 0;             return; }
    else if (scancode == 0x3A) { caps_lock_active = !caps_lock_active;              return; }

    // Release codes (bit 7 set) — discard after modifier handling
    if (scancode & 0x80) return;

    // Ctrl-combos: send signal to foreground process group
    if (ctrl_pressed) {
        uint32_t fg = terminal_fg_pid;
        if (fg) {
            if (scancode == SCANCODE_C) {
                task_signal(fg, SIGINT);
                return;
            }
            if (scancode == SCANCODE_BSLASH) {
                task_signal(fg, SIGQUIT);
                return;
            }
        }
    }

    // Translate scancode → character with Shift/Caps Lock handling
    char c = keymap[scancode];
    if (!c) return;

    if (c >= 'a' && c <= 'z') {
        // XOR: if exactly one of Shift or Caps Lock is active → uppercase
        if (shift_pressed != caps_lock_active)
            c = keymap_shift[scancode];
    } else if (shift_pressed) {
        c = keymap_shift[scancode];
    }

    keyboard_post_key(c);
}