#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stdint.h>

extern volatile char          last_char;
extern volatile int           key_event_happened;
extern volatile int           keyboard_irq_count;
extern volatile unsigned char last_scancode_raw;

void keyboard_post_key(char c);

#endif