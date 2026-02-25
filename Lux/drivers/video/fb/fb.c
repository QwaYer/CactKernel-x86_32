#include "fb.h"

static uint32_t* fb_buffer = 0;
static uint32_t fb_width   = 0;
static uint32_t fb_height  = 0;
static uint32_t fb_pitch   = 0;
static uint8_t  fb_bpp     = 0;

void fb_init(multiboot_info_t* mbi) {
    if (!(mbi->flags & (1 << 12))) {
        fb_width = 0;
        return;
    }

    if ((mbi->framebuffer_addr >> 32) != 0) {
        fb_width = 0;
        return;
    }

    uint32_t addr = (uint32_t)(mbi->framebuffer_addr & 0xFFFFFFFF);

    if (mbi->framebuffer_type != 1) {
        fb_width = 0;
        return;
    }

    if (mbi->framebuffer_bpp != 32) {
        fb_width = 0;
        return;
    }

    if (addr == 0 || mbi->framebuffer_width == 0 || mbi->framebuffer_height == 0) {
        fb_width = 0;
        return;
    }

    fb_buffer = (uint32_t*)(uintptr_t)addr;
    fb_width  = mbi->framebuffer_width;
    fb_height = mbi->framebuffer_height;
    fb_pitch  = mbi->framebuffer_pitch;
    fb_bpp    = mbi->framebuffer_bpp;
}

void fb_put_pixel(uint32_t x, uint32_t y, uint32_t color) {
    if (!fb_buffer || x >= fb_width || y >= fb_height) return;

    uint32_t offset = y * (fb_pitch / 4) + x;
    fb_buffer[offset] = color;
}

void fb_fill_rect(uint32_t x, uint32_t y,
                  uint32_t width, uint32_t height, uint32_t color) {
    if (x >= fb_width || y >= fb_height) return;
    if (x + width  > fb_width)  width  = fb_width  - x;
    if (y + height > fb_height) height = fb_height - y;

    uint32_t words_per_row = fb_pitch / 4;
    for (uint32_t row = 0; row < height; row++) {
        uint32_t* line = fb_buffer + (y + row) * words_per_row + x;
        for (uint32_t col = 0; col < width; col++) {
            line[col] = color;
        }
    }
}

void fb_clear(uint32_t color) {
    if (!fb_buffer || fb_width == 0 || fb_height == 0) return;
    fb_fill_rect(0, 0, fb_width, fb_height, color);
}

uint32_t  fb_get_width()  { return fb_width;  }
uint32_t  fb_get_height() { return fb_height; }
uint32_t  fb_get_pitch()  { return fb_pitch;  }
uint32_t* fb_get_buffer() { return fb_buffer; }