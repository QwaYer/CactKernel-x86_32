#ifndef PS2_KEYBOARD_H
#define PS2_KEYBOARD_H

// Initialise PS/2 controller, enable IRQ1
int  ps2_keyboard_init(void);

// IRQ1 handler — called from interrupt stub
void ps2_keyboard_handler(void);

#endif