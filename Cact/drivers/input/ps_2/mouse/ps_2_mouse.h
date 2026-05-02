#ifndef PS2_MOUSE_H
#define PS2_MOUSE_H

// Initialise auxiliary PS/2 port, enable IRQ12, start data reporting
void ps2_mouse_init(void);

// IRQ12 handler — called from interrupt stub
void ps2_mouse_handler(void);

#endif