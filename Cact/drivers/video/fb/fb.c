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
static fb_init_result_t fb_last_status = FB_INIT_OK;

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

void fb_copy32(uint32_t* dst, const uint32_t* src, uint32_t n_words) {
    __builtin_prefetch(src, 0, 3);
    __asm__ __volatile__ ("rep movsl"
        : "+D"(dst), "+S"(src), "+c"(n_words)
        :
        : "memory");
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

    if (mbi->framebuffer_bpp != 32) {
        fb_width = 0;
        fb_last_status = FB_INIT_BAD_BPP;
        return FB_INIT_BAD_BPP;
    }

    if (addr == 0 || mbi->framebuffer_width == 0 || mbi->framebuffer_height == 0) {
        fb_width = 0;
        fb_last_status = FB_INIT_NULL_PARAM;
        return FB_INIT_NULL_PARAM;
    }

    fb_buffer = (uint32_t*)(uintptr_t)addr;
    fb_width  = mbi->framebuffer_width;
    fb_height = mbi->framebuffer_height;
    fb_pitch  = mbi->framebuffer_pitch;
    fb_bpp    = mbi->framebuffer_bpp;
    fb_last_status = FB_INIT_OK;
    return FB_INIT_OK;
}

fb_init_result_t fb_get_init_status(void) {
    return fb_last_status;
}

void fb_put_pixel(uint32_t x, uint32_t y, uint32_t color) {
    if (unlikely(!fb_buffer || x >= fb_width || y >= fb_height))
        return;

    uint32_t wpr = fb_pitch / 4u;
    if (unlikely(wpr == 0))
        return;

    fb_render_buf()[(size_t)y * (size_t)wpr + (size_t)x] = color;
    fb_mark_dirty_row(y);
}

void fb_fill_rect(uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint32_t color) {
    if (unlikely(x >= fb_width || y >= fb_height))
        return;
    if ((uint64_t)x + (uint64_t)width > (uint64_t)fb_width)
        width = fb_width - x;
    if ((uint64_t)y + (uint64_t)height > (uint64_t)fb_height)
        height = fb_height - y;

    uint32_t wpr = fb_pitch / 4u;
    if (unlikely(wpr == 0 || width == 0 || height == 0))
        return;

    uint32_t* buf = fb_render_buf();

    /* Full‑width rect: rows are contiguous — single REP STOSD over all
     * scanlines instead of one per row.  This is the common case for
     * scroll() (clear bottom band) and fb_clear(). */
    if (likely(x == 0 && width == wpr)) {
        uint32_t* start  = buf + (size_t)y * (size_t)wpr;
        uint32_t  total  = height * wpr;
        __asm__ __volatile__ ("rep stosl" : "+D"(start), "+c"(total) : "a"(color) : "memory");
    } else {
        uint32_t* row_ptr = buf + (size_t)y * (size_t)wpr + (size_t)x;
        uint32_t  full_w  = width;
        for (uint32_t row = 0; row < height; row++) {
            uint32_t* line = row_ptr;
            uint32_t  n    = full_w;
            __asm__ __volatile__ ("rep stosl" : "+D"(line), "+c"(n) : "a"(color) : "memory");
            row_ptr += wpr;
        }
    }
    fb_mark_dirty_rows(y, y + height);
}

void fb_clear(uint32_t color) {
    if (unlikely(!fb_buffer || fb_width == 0 || fb_height == 0))
        return;
    uint32_t* buf = fb_render_buf();
    uint32_t count = (fb_pitch / 4u) * fb_height;
    if (unlikely(count == 0)) return;
    uint32_t* dst = buf;
    __asm__ __volatile__ ("rep stosl" : "+D"(dst), "+c"(count) : "a"(color) : "memory");
    fb_mark_dirty_rows(0, fb_height);
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

    uint32_t wpr = fb_pitch / 4u;
    if (wpr == 0) return;

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
        uint32_t words   = run_len * wpr;
        /* Prefetch the shadow rows ahead so the CPU can start the
         * WB→L1 transfer while the current MOVSD is in flight. */
        __builtin_prefetch(fb_shadow + (size_t)(run_start + 4) * (size_t)wpr, 0, 2);
        uint32_t* dst = fb_buffer + (size_t)run_start * (size_t)wpr;
        uint32_t* src = fb_shadow + (size_t)run_start * (size_t)wpr;
        __asm__ __volatile__ ("rep movsl"
            : "+D"(dst), "+S"(src), "+c"(words)
            :
            : "memory");
    }
    fb_dirty_y_min = fb_height;
    fb_dirty_y_max = 0;
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
uint32_t* fb_get_buffer(void) {
    return fb_buffer;
}
