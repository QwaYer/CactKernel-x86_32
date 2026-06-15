#include "serial.h"
#include "kernel.h"

#define COM1 0x3F8u

static void com_wr(unsigned reg, uint8_t v) {
    port_byte_out((uint16_t)(COM1 + reg), v);
}

static uint8_t com_rd(unsigned reg) {
    return (uint8_t)(port_byte_in((uint16_t)(COM1 + reg)) & 0xFFu);
}

void serial_init(void) {
    com_wr(1, 0x00); /* no IRQ */
    com_wr(3, 0x80); /* DLAB */
    com_wr(0, 0x01); /* divisor low — 115200 */
    com_wr(1, 0x00); /* divisor high */
    com_wr(3, 0x03); /* 8N1 */
    com_wr(2, 0xC7); /* FIFO on */
    com_wr(4, 0x0B); /* RTS/DTR */
}

void serial_putc(char c) {
    while (!(com_rd(5) & 0x20u))
        ;
    com_wr(0, (uint8_t)c);
}
