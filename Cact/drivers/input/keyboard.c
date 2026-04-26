#include "keyboard.h"

volatile char          last_char          = 0;
volatile int           key_event_happened = 0;
volatile int           keyboard_irq_count = 0;
volatile unsigned char last_scancode_raw  = 0;

static volatile char     kb_buf[KB_BUF_SIZE];
static volatile uint32_t kb_wr = 0;
static volatile uint32_t kb_rd = 0;

void keyboard_post_key(char c) {
    if (!c) return;
    uint32_t next = (kb_wr + 1) % KB_BUF_SIZE;
    if (next != kb_rd) {
        kb_buf[kb_wr] = c;
        kb_wr = next;
    }
    last_char          = c;
    key_event_happened = 1;
    keyboard_irq_count++;
}

int keyboard_read_char(void) {
    if (kb_rd == kb_wr) return -1;
    char c = kb_buf[kb_rd];
    kb_rd = (kb_rd + 1) % KB_BUF_SIZE;
    return (unsigned char)c;
}