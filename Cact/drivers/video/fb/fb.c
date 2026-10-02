#include "fb.h"
#include "fb_internal.h"
#include "klib.h"
#include "memory.h"
#include "serial.h"
#include "kernel.h"
#include <stddef.h>
#include <stdarg.h>

uint32_t* fb_buffer      = 0;
uint32_t  fb_width       = 0;
uint32_t  fb_height      = 0;
uint32_t  fb_pitch       = 0;
uint8_t   fb_bpp         = 0;
uint8_t   fb_bytespp     = 0;
static fb_init_result_t fb_last_status = FB_INIT_OK;

/* Direct-colour channel layout, taken verbatim from the bootloader's
 * framebuffer tag so a pixel is packed the way this mode expects. */
static uint8_t fb_red_pos = 16, fb_red_size = 8;
static uint8_t fb_green_pos = 8, fb_green_size = 8;
static uint8_t fb_blue_pos = 0,  fb_blue_size = 8;

/* ------------------------------------------------------------------------ *
 *  Shadow buffer (optional back buffer in WB kernel RAM)
 *
 *  fb_shadow       Page-aligned heap copy of the framebuffer; same pitch and
 *                  height as the real FB. All drawing primitives write here
 *                  while the shadow is armed.
 *  fb_dirty_row    1 byte per scanline; non-zero = needs to be shipped out
 *                  to the real framebuffer on the next fb_flush().
 *  fb_dirty_y_min  Envelope of dirty rows, so fb_flush() can skip the rest
 *  fb_dirty_y_max  of the screen in O(1) when only a few rows changed.
 *  fb_shadow_armed Latched after a successful fb_enable_shadow() call.
 * ------------------------------------------------------------------------ */
uint32_t* fb_shadow       = 0;
uint8_t*  fb_dirty_row    = 0;
uint32_t  fb_dirty_y_min  = 0;
uint32_t  fb_dirty_y_max  = 0;
int       fb_shadow_armed = 0;

/* Returns the buffer the drawing primitives should mutate.  When the shadow
 * is armed, all writes go to WB RAM (cache-speed); otherwise they fall
 * through to the real (possibly WC) framebuffer at MMIO. */
uint32_t* fb_render_buf(void) {
    return likely(fb_shadow_armed) ? fb_shadow : fb_buffer;
}

void fb_mark_dirty_row(uint32_t y) {
    if (unlikely(!fb_shadow_armed)) return;
    if (unlikely(y >= fb_height))   return;
    fb_dirty_row[y] = 1;
    if (y < fb_dirty_y_min) fb_dirty_y_min = y;
    if (y > fb_dirty_y_max) fb_dirty_y_max = y;
}

void fb_mark_dirty_rows(uint32_t y0, uint32_t y1) {
    if (unlikely(!fb_shadow_armed)) return;
    if (unlikely(y0 >= fb_height))  return;
    if (y1 > fb_height)   y1 = fb_height;
    if (y1 <= y0)         return;
    {
        uint32_t n = y1 - y0;
        uint8_t* p = fb_dirty_row + y0;
        __asm__ __volatile__ ("rep stosb" : "+D"(p), "+c"(n) : "a"(1) : "memory");
    }
    if (y0     < fb_dirty_y_min) fb_dirty_y_min = y0;
    if (y1 - 1 > fb_dirty_y_max) fb_dirty_y_max = y1 - 1;
}

/* Scale an 8-bit channel value into the field the hardware uses. */
static inline uint32_t fb_pack_channel(uint32_t v8, uint32_t pos, uint32_t size) {
    if (size == 0) return 0;
    if (size < 8)      v8 >>= (8 - size);
    else if (size > 8) v8 <<= (size - 8);
    return v8 << pos;
}

uint32_t fb_pack_color(uint32_t rgb) {
    return fb_pack_channel((rgb >> 16) & 0xFFu, fb_red_pos,   fb_red_size)
         | fb_pack_channel((rgb >> 8)  & 0xFFu, fb_green_pos, fb_green_size)
         | fb_pack_channel( rgb        & 0xFFu, fb_blue_pos,  fb_blue_size);
}

fb_init_result_t fb_init(multiboot_info_t* mbi) {
    if (!(mbi->flags & (1 << 12))) {
        fb_width = 0;
        fb_last_status = FB_INIT_NO_FLAG;
        return FB_INIT_NO_FLAG;
    }

    if ((mbi->framebuffer_addr >> 32) != 0) {
        fb_width = 0;
        fb_last_status = FB_INIT_HIGH_ADDR;
        return FB_INIT_HIGH_ADDR;
    }

    uint32_t addr = (uint32_t)(mbi->framebuffer_addr & 0xFFFFFFFF);

    if (mbi->framebuffer_type != 1) {
        fb_width = 0;
        fb_last_status = FB_INIT_BAD_TYPE;
        return FB_INIT_BAD_TYPE;
    }

    /* The pixel depth is whatever the bootloader chose; only whole-byte
     * depths up to 32 bpp are representable by the rasteriser. */
    uint8_t bpp = mbi->framebuffer_bpp;
    if (bpp != 8 && bpp != 15 && bpp != 16 && bpp != 24 && bpp != 32) {
        fb_width = 0;
        fb_last_status = FB_INIT_BAD_BPP;
        return FB_INIT_BAD_BPP;
    }

    if (addr == 0 || mbi->framebuffer_width == 0 || mbi->framebuffer_height == 0) {
        fb_width = 0;
        fb_last_status = FB_INIT_NULL_PARAM;
        return FB_INIT_NULL_PARAM;
    }

    /* 15 bpp is a 5:5:5 mode packed into 16 bits, so round up to whole bytes. */
    uint8_t bytespp = (uint8_t)((bpp + 7u) / 8u);
    if ((uint64_t)mbi->framebuffer_pitch < (uint64_t)mbi->framebuffer_width * bytespp) {
        fb_width = 0;
        fb_last_status = FB_INIT_BAD_PITCH;
        return FB_INIT_BAD_PITCH;
    }

    fb_buffer = (uint32_t*)(uintptr_t)addr;
    fb_width  = mbi->framebuffer_width;
    fb_height = mbi->framebuffer_height;
    fb_pitch  = mbi->framebuffer_pitch;
    fb_bpp    = bpp;
    fb_bytespp= bytespp;

    fb_red_pos    = mbi->framebuffer_red_pos;
    fb_red_size   = mbi->framebuffer_red_size;
    fb_green_pos  = mbi->framebuffer_green_pos;
    fb_green_size = mbi->framebuffer_green_size;
    fb_blue_pos   = mbi->framebuffer_blue_pos;
    fb_blue_size  = mbi->framebuffer_blue_size;
    /* Guard against a tag that carried no channel info at all. */
    if (fb_red_size == 0 && fb_green_size == 0 && fb_blue_size == 0) {
        fb_red_pos = 16; fb_red_size = 8;
        fb_green_pos = 8; fb_green_size = 8;
        fb_blue_pos = 0;  fb_blue_size = 8;
    }

    fb_last_status = FB_INIT_OK;
    return FB_INIT_OK;
}

fb_init_result_t fb_get_init_status(void) {
    return fb_last_status;
}

void fb_put_pixel(uint32_t x, uint32_t y, uint32_t color) {
    if (unlikely(!fb_buffer || x >= fb_width || y >= fb_height))
        return;
    if (unlikely(fb_bytespp == 0))
        return;

    uint8_t* p = (uint8_t*)fb_render_buf()
               + (size_t)y * (size_t)fb_pitch
               + (size_t)x * (size_t)fb_bytespp;
    fb_store_pixel(p, fb_pack_color(color));
    fb_mark_dirty_row(y);
}

void fb_fill_rect(uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint32_t color) {
    if (unlikely(x >= fb_width || y >= fb_height))
        return;
    if ((uint64_t)x + (uint64_t)width > (uint64_t)fb_width)
        width = fb_width - x;
    if ((uint64_t)y + (uint64_t)height > (uint64_t)fb_height)
        height = fb_height - y;
    if (unlikely(fb_bytespp == 0 || width == 0 || height == 0))
        return;

    const uint32_t px = fb_pack_color(color);
    uint8_t* row = (uint8_t*)fb_render_buf()
                 + (size_t)y * (size_t)fb_pitch
                 + (size_t)x * (size_t)fb_bytespp;

    for (uint32_t r = 0; r < height; r++) {
        uint8_t* p = row;
        for (uint32_t c = 0; c < width; c++) {
            fb_store_pixel(p, px);
            p += fb_bytespp;
        }
        row += fb_pitch;
    }
    fb_mark_dirty_rows(y, y + height);
}

void fb_clear(uint32_t color) {
    if (unlikely(!fb_buffer || fb_width == 0 || fb_height == 0))
        return;
    fb_fill_rect(0, 0, fb_width, fb_height, color);
}

/* ------------------------------------------------------------------------ *
 *  Shadow buffer public API
 * ------------------------------------------------------------------------ */

void fb_enable_shadow(void) {
    if (fb_shadow_armed) return;
    if (!fb_buffer || fb_width == 0 || fb_height == 0) return;

    /* Allocate the shadow page-aligned: gives us cache-line-friendly rows
     * and lets us upgrade to SSE/AVX memcpy later without re-allocating. */
    size_t fb_pitch_s = (size_t)fb_pitch;
    size_t fb_height_s = (size_t)fb_height;
    if (fb_pitch_s > 0 && fb_height_s > SIZE_MAX / fb_pitch_s) {
        pr_warn("  %-11s : pitch*height overflow — staying in direct mode\n", "fb");
        return;
    }
    size_t shadow_bytes = fb_pitch_s * fb_height_s;
    uint32_t* shadow = (uint32_t*)kmalloc_aligned((uint32_t)shadow_bytes, 4096);
    if (!shadow) {
        pr_warn("  %-11s : shadow buffer alloc failed — direct mode\n", "fb");
        return;
    }

    uint8_t* dirty = (uint8_t*)kmalloc(fb_height);
    if (!dirty) {
        kfree(shadow);
        pr_warn("  %-11s : dirty bitmap alloc failed\n", "fb");
        return;
    }
    memset(dirty, 0, fb_height);

    /* Seed the shadow with whatever is on screen right now so the boot
     * transcript drawn before this point stays intact. The read side of
     * this memcpy is WC (uncached) -- one-time tens-of-ms hit on 1080p,
     * acceptable as a boot-time cost. */
    memcpy(shadow, fb_buffer, shadow_bytes);

    fb_shadow       = shadow;
    fb_dirty_row    = dirty;
    fb_dirty_y_min  = fb_height;   /* sentinel meaning "clean"               */
    fb_dirty_y_max  = 0;
    fb_shadow_armed = 1;
    /* "… + WB shadow ready" is reported once by kernel.c after boot setup. */
}

void fb_flush(void) {
    if (unlikely(!fb_shadow_armed))                     return;
    if (unlikely(!fb_buffer || !fb_shadow || !fb_dirty_row)) return;
    if (fb_dirty_y_min > fb_dirty_y_max)      return;

    uint32_t y_lo = fb_dirty_y_min;
    uint32_t y_hi = fb_dirty_y_max;
    if (y_hi >= fb_height) y_hi = fb_height - 1;

    uint32_t y = y_lo;
    while (y <= y_hi) {
        if (!fb_dirty_row[y]) { y++; continue; }
        uint32_t run_start = y;
        while (y <= y_hi && fb_dirty_row[y]) {
            fb_dirty_row[y] = 0;
            y++;
        }
        uint32_t run_len = y - run_start;
        /* Prefetch the shadow rows ahead so the CPU can start the
         * WB→L1 transfer while the current copy is in flight. */
        __builtin_prefetch(fb_shadow + (size_t)(run_start + 4) * (size_t)fb_pitch, 0, 2);
        uint8_t* dst = (uint8_t*)fb_buffer + (size_t)run_start * (size_t)fb_pitch;
        uint8_t* src = (uint8_t*)fb_shadow + (size_t)run_start * (size_t)fb_pitch;
        memcpy(dst, src, run_len * fb_pitch);
    }
    fb_dirty_y_min = fb_height;
    fb_dirty_y_max = 0;
}

void fb_repaint(void) {
    fb_mark_dirty_rows(0, fb_height);
    fb_flush();
}

uint32_t fb_get_width(void) {
    return fb_width;
}
uint32_t fb_get_height(void) {
    return fb_height;
}
uint32_t fb_get_pitch(void) {
    return fb_pitch;
}
uint8_t fb_get_bpp(void) {
    return fb_bpp;
}
uint32_t* fb_get_buffer(void) {
    return fb_buffer;
}
uint32_t fb_get_red_pos(void) {
    return fb_red_pos;
}
