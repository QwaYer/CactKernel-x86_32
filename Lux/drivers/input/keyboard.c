#include "keyboard.h"
#include "kernel.h"

static unsigned char keyboard_map[128] = {
    0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0,
    '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' '
};

static unsigned char keyboard_map_shift[128] = {
    0,  27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0,
    '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' '
};

static int shift_pressed   = 0;
static int caps_lock_active = 0;

volatile char          last_char         = 0;
volatile int           key_event_happened = 0;
volatile int           keyboard_irq_count = 0;
volatile unsigned char last_scancode_raw  = 0;

int init_keyboard() {
    while (port_byte_in(0x64) & 0x02);
    port_byte_out(0x64, 0xAE);
    while (port_byte_in(0x64) & 0x01) port_byte_in(0x60);
    return 0;
}

void keyboard_handler() {
    unsigned char scancode = port_byte_in(0x60);

    last_scancode_raw = scancode;
    keyboard_irq_count++;

    unsigned char* vram = (unsigned char*)VIDEO_ADDRESS;
    unsigned int base = (0 * 80 + 72) * 2;
    unsigned char hi = (keyboard_irq_count >> 4) & 0xF;
    unsigned char lo =  keyboard_irq_count        & 0xF;
    vram[base + 0] = (hi < 10) ? ('0' + hi) : ('A' + hi - 10);
    vram[base + 1] = 0x4F; 
    vram[base + 2] = (lo < 10) ? ('0' + lo) : ('A' + lo - 10);
    vram[base + 3] = 0x4F;
    vram[base + 4] = ':';
    vram[base + 5] = 0x4F;
    hi = (scancode >> 4) & 0xF;
    lo =  scancode        & 0xF;
    vram[base + 6] = (hi < 10) ? ('0' + hi) : ('A' + hi - 10);
    vram[base + 7] = 0x4F;
    vram[base + 8] = (lo < 10) ? ('0' + lo) : ('A' + lo - 10);
    vram[base + 9] = 0x4F;

    if      (scancode == 0x2A || scancode == 0x36) { shift_pressed = 1; return; }
    else if (scancode == 0xAA || scancode == 0xB6) { shift_pressed = 0; return; }
    else if (scancode == 0x3A) { caps_lock_active = !caps_lock_active; return; }

    if (scancode & 0x80) return;

    char c = keyboard_map[scancode];
    if (c == 0) return;

    if (c >= 'a' && c <= 'z') {
        if (shift_pressed != caps_lock_active)
            c = keyboard_map_shift[scancode];
    } else if (shift_pressed) {
        c = keyboard_map_shift[scancode];
    }

    if (c != 0) {
        last_char          = c;
        key_event_happened = 1;
    }
}

void backspace_visual_update() {
    extern int cursor_x;
    extern int cursor_y;

    cursor_x--;
    if (cursor_x < 0) {
        cursor_x = MAX_COLS - 1;
        cursor_y--;
        if (cursor_y < 0) { cursor_x = 0; cursor_y = 0; return; }
    }

    int offset = (cursor_y * MAX_COLS + cursor_x) * 2;
    unsigned char* video_mem = (unsigned char*)VIDEO_ADDRESS;
    video_mem[offset]     = ' ';
    video_mem[offset + 1] = WHITE_ON_BLACK;
    update_hardware_cursor(cursor_x, cursor_y);
}