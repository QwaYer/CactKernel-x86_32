#include "keyboard.h"

// Volatile globals — written by IRQ handler, read by userspace
volatile char          last_char          = 0;
volatile int           key_event_happened = 0;

// Circular buffer — protected by single-producer/single-consumer ordering
static volatile char     kb_buf[KB_BUF_SIZE];
static volatile uint32_t kb_wr = 0;   // IRQ handler writes here
static volatile uint32_t kb_rd = 0;   // terminal task reads here

// Push a character into the buffer (called from IRQ context).
// Drops the character if buffer is full (non-blocking).
void keyboard_post_key(char c) {
    if (!c) return;
    uint32_t next = (kb_wr + 1) % KB_BUF_SIZE;
    if (next != kb_rd) {              // not full
        kb_buf[kb_wr] = c;
        __asm__ __volatile__("" ::: "memory");   // barrier: data visible before wr update
        kb_wr = next;
    }
    last_char          = c;
    key_event_happened = 1;
}

// Pop a character from the buffer (called from userspace).
// Returns -1 if buffer is empty.
int keyboard_read_char(void) {
    if (kb_rd == kb_wr) return -1;    // empty
    __asm__ __volatile__("" ::: "memory");   // barrier: read data after reading kb_wr
    char c = kb_buf[kb_rd];
    kb_rd = (kb_rd + 1) % KB_BUF_SIZE;
    return (unsigned char)c;
}