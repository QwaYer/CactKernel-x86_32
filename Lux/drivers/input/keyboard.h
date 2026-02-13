#ifndef KEYBOARD_H
#define KEYBOARD_H

extern volatile char    last_char;
extern volatile int     key_event_happened;
extern volatile int     keyboard_irq_count;
extern volatile unsigned char last_scancode_raw;

int  init_keyboard();
void keyboard_handler();
void backspace_visual_update();

#endif