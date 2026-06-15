#include "mouse.h"
#include "fb.h"

// Global mouse state — read by userspace GUI or terminal
int mouse_x       = 0;
int mouse_y       = 0;
int mouse_buttons = 0;

// Circular buffer for mouse events — single-producer (IRQ) / single-consumer (userspace)
static volatile mouse_packet_t mouse_buf[MOUSE_BUF_SIZE];
static volatile uint32_t   mouse_wr = 0;   // IRQ handler writes here
static volatile uint32_t   mouse_rd = 0;   // userspace reads here

// Enqueue a mouse event (called from IRQ context). Drops on overflow.
static void mouse_enqueue(int dx, int dy, int buttons, int absolute) {
    uint32_t next = (mouse_wr + 1) % MOUSE_BUF_SIZE;
    if (next == mouse_rd) return;   // full
    mouse_buf[mouse_wr].dx       = dx;
    mouse_buf[mouse_wr].dy       = dy;
    mouse_buf[mouse_wr].buttons  = buttons;
    mouse_buf[mouse_wr].absolute = absolute;
    mouse_wr = next;
}

// Dequeue one mouse event; returns 0 on success, -1 if buffer is empty.
int mouse_read_event(mouse_packet_t *pkt) {
    if (mouse_rd == mouse_wr) return -1;
    *pkt = mouse_buf[mouse_rd];
    mouse_rd = (mouse_rd + 1) % MOUSE_BUF_SIZE;
    return 0;
}

// Update mouse position from a movement packet.
// Relative mode (PS/2): adds delta; absolute mode (USB HID): sets directly.
// Coordinates are clamped to framebuffer dimensions.
void mouse_post_move(int x, int y, int buttons, int absolute) {
    uint32_t w = fb_get_width();
    uint32_t h = fb_get_height();

    // Enqueue raw event BEFORE clamping so relative deltas are preserved
    mouse_enqueue(x, y, buttons & 0x07, absolute ? 1 : 0);

    if (absolute) {
        mouse_x = x;
        mouse_y = y;
    } else {
        if ((x > 0 && mouse_x > (int)0x7FFFFFFF - x) ||
            (x < 0 && mouse_x < (int)(-2147483647 - 1) - x))
            mouse_x = (x > 0) ? (int)0x7FFFFFFF : (int)(-2147483647 - 1);
        else
            mouse_x += x;

        if ((y > 0 && mouse_y > (int)0x7FFFFFFF - y) ||
            (y < 0 && mouse_y < (int)(-2147483647 - 1) - y))
            mouse_y = (y > 0) ? (int)0x7FFFFFFF : (int)(-2147483647 - 1);
        else
            mouse_y += y;
    }

    // Clamp to screen bounds
    if (mouse_x < 0)       mouse_x = 0;
    if (mouse_y < 0)       mouse_y = 0;
    if (mouse_x >= (int)w) mouse_x = (int)w - 1;
    if (mouse_y >= (int)h) mouse_y = (int)h - 1;

    // Only bits 0-2 are valid buttons (left, right, middle)
    mouse_buttons = buttons & 0x07;
}