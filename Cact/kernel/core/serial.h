#ifndef SERIAL_H
#define SERIAL_H

// NS16550 COM1 — mirrors kernel console to serial (QEMU: -serial stdio). 
void serial_init(void);
void serial_putc(char c);

#endif
