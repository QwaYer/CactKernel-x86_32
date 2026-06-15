#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stdint.h>

// Debug/diagnostic globals — updated by IRQ handler
extern volatile char          last_char;
extern volatile int           key_event_happened;

// Circular buffer size (must be power of 2 for efficient wrapping)
#define KB_BUF_SIZE 256

// Called from IRQ handler to enqueue a character
void keyboard_post_key(char c);

// Called from userspace to dequeue a character; returns -1 if empty
int  keyboard_read_char(void);

#endif