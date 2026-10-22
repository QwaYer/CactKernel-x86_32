#include "serial.h"
#include "kernel.h"

#define COM1 0x3F8u

static void com_wr(unsigned reg, uint8_t v) {
    outb((uint16_t)(COM1 + reg), v);
}

static uint8_t com_rd(unsigned reg) {
    return (uint8_t)(inb((uint16_t)(COM1 + reg)) & 0xFFu);
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
    /* Never wait for THRE: spinning here caps every console write at the line
     * rate (115200 baud ≈ 11.5 KB/s), which a full-screen TUI redraw turns into
     * hundreds of ms of lag per keystroke.  A byte the UART cannot take right
     * now is dropped — QEMU's emulated FIFO drains instantly, so the headless
     * -serial transcript is unaffected. */
    if (com_rd(5) & 0x20u)
        com_wr(0, (uint8_t)c);
}
