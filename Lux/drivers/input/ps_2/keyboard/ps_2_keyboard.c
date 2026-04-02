#include "ps_2_keyboard.h"
#include "keyboard.h"
#include "kernel.h"
#include "task.h"

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
static int ctrl_pressed     = 0;

/* PS/2 set-1 scancodes for keys used in Ctrl-combos */
#define SCANCODE_C    0x2E   /* 'c' — Ctrl-C → SIGINT  */
#define SCANCODE_BSLASH 0x2B /* '\' — Ctrl-\ → SIGQUIT */

int ps2_keyboard_init(void) {
    kprint("[KBD] flushing output buffer (port 0x60)\n");
    while (port_byte_in(0x64) & 0x01)
        port_byte_in(0x60);

    kprint("[KBD] enabling first PS/2 port (cmd 0xAE)\n");
    while (port_byte_in(0x64) & 0x02);
    port_byte_out(0x64, 0xAE);

    kprint("[KBD] reading controller config byte (cmd 0x20)\n");
    while (port_byte_in(0x64) & 0x02);
    port_byte_out(0x64, 0x20);
    while (!(port_byte_in(0x64) & 0x01));
    unsigned char config = port_byte_in(0x60);
    config |= 0x01;  // enable IRQ1

    kprint("[KBD] writing config byte (IRQ1 enabled) via cmd 0x60\n");
    while (port_byte_in(0x64) & 0x02);
    port_byte_out(0x64, 0x60);
    while (port_byte_in(0x64) & 0x02);
    port_byte_out(0x60, config);

    kprint("[KBD] flushing residual bytes\n");
    while (port_byte_in(0x64) & 0x01)
        port_byte_in(0x60);

    kprint("[KBD] ready — IRQ1 will fire on each keypress\n");
    return 0;
}

void ps2_keyboard_handler(void) {
    unsigned char scancode = port_byte_in(0x60);

    if      (scancode == 0x2A || scancode == 0x36) { shift_pressed = 1;             return; }
    else if (scancode == 0xAA || scancode == 0xB6) { shift_pressed = 0;             return; }
    else if (scancode == 0x1D)                     { ctrl_pressed  = 1;             return; }
    else if (scancode == 0x9D)                     { ctrl_pressed  = 0;             return; }
    else if (scancode == 0x3A) { caps_lock_active = !caps_lock_active;              return; }

    if (scancode & 0x80) return;

    /* TTY-level signal generation: Ctrl-C → SIGINT, Ctrl-\ → SIGQUIT */
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