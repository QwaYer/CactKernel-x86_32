#include "keyboard.h"

volatile char          last_char          = 0;
volatile int           key_event_happened = 0;
volatile int           keyboard_irq_count = 0;
volatile unsigned char last_scancode_raw  = 0;

void keyboard_post_key(char c) {
    if (!c) return;
    last_char          = c;
    key_event_happened = 1;
    keyboard_irq_count++;
}