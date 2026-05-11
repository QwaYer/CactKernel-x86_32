#include "fb.h"
#include "klib.h"
#include "memory.h"
#include "serial.h"
#include <stddef.h>

static int cursor_x = 0;
static int cursor_y = 0;

static uint32_t*        fb_buffer      = 0;
static uint32_t         fb_width       = 0;
static uint32_t         fb_height      = 0;
static uint32_t         fb_pitch       = 0;
static uint8_t          fb_bpp         = 0;
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
static uint32_t*        fb_shadow       = 0;
static uint8_t*         fb_dirty_row    = 0;
static uint32_t         fb_dirty_y_min  = 0;
static uint32_t         fb_dirty_y_max  = 0;
static int              fb_shadow_armed = 0;

/* Returns the buffer the drawing primitives should mutate.  When the shadow
 * is armed, all writes go to WB RAM (cache-speed); otherwise they fall
 * through to the real (possibly WC) framebuffer at MMIO. */
static inline uint32_t* fb_render_buf(void) {
    return fb_shadow_armed ? fb_shadow : fb_buffer;
}

static inline void fb_mark_dirty_row(uint32_t y) {
    if (!fb_shadow_armed) return;
    if (y >= fb_height)   return;
    fb_dirty_row[y] = 1;
    if (y < fb_dirty_y_min) fb_dirty_y_min = y;
    if (y > fb_dirty_y_max) fb_dirty_y_max = y;
}

static inline void fb_mark_dirty_rows(uint32_t y0, uint32_t y1) {
    if (!fb_shadow_armed) return;
    if (y0 >= fb_height)  return;
    if (y1 > fb_height)   y1 = fb_height;
    if (y1 <= y0)         return;
    for (uint32_t y = y0; y < y1; y++) fb_dirty_row[y] = 1;
    if (y0     < fb_dirty_y_min) fb_dirty_y_min = y0;
    if (y1 - 1 > fb_dirty_y_max) fb_dirty_y_max = y1 - 1;
}

/* Fast 32-bit-aligned forward copy via REP MOVSD. The framebuffer (and
 * its shadow) are always 4-byte aligned and have a 4-byte-multiple pitch,
 * so this is strictly faster than the kernel's byte-wise libc memcpy()
 * fallback. Safe for the dst < src overlap case used by scroll(). */
static inline void fb_copy32(uint32_t* dst, const uint32_t* src, uint32_t n_words) {
    __asm__ __volatile__ ("rep movsl"
        : "+D"(dst), "+S"(src), "+c"(n_words)
        :
        : "memory");
}

/* Rasterise one glyph at (px, py) directly into the shadow (or, when the
 * shadow is inactive, the real framebuffer). Builds one scaled scanline
 * in a small stack buffer and stamps it down `scale` times per source
 * row — eliminates the FONT_HEIGHT*FONT_WIDTH*scale^2 per-pixel function
 * calls the previous implementation paid (~256 calls per 16x16 glyph),
 * each of which re-did the bounds check and the dirty-bitmap update. */
static void fb_draw_char_scaled(char c, int px, int py, uint32_t color) {
    if ((unsigned char)c >= 128) return;

    const uint32_t out_w = (uint32_t)FB_CONSOLE_CHAR_WIDTH;   /* 8*scale */
    const uint32_t out_h = (uint32_t)FB_CONSOLE_CHAR_HEIGHT;

    /* The console code never asks us to draw partially off-screen, so a
     * single envelope check at entry is enough — drop the per-pixel
     * bounds check entirely from the inner loop. */
    if (px < 0 || py < 0)                    return;
    if ((uint32_t)px + out_w > fb_width)     return;
    if ((uint32_t)py + out_h > fb_height)    return;

    uint32_t wpr = fb_pitch / 4u;
    if (wpr == 0) return;

    uint32_t* buf = fb_render_buf();
    if (!buf) return;

    const uint8_t* glyph = font8x8_basic[(unsigned char)c];

    /* One scaled output scanline, materialised on the stack. 8 source
     * pixels * 2 (scale) = 16 u32 = 64 bytes — fits in a cache line. */
    uint32_t scanline[FB_CONSOLE_CHAR_WIDTH];

    for (int row = 0; row < FONT_HEIGHT; row++) {
        uint8_t bits = glyph[row];

        /* Build the scaled scanline once per source row. */
        uint32_t* p = scanline;
        for (int col = 0; col < FONT_WIDTH; col++) {
            uint32_t pix = (bits & (1u << col)) ? color : COLOR_BLACK;
            for (int sx = 0; sx < FB_CONSOLE_FONT_SCALE; sx++)
                *p++ = pix;
        }

        /* Stamp the scanline `scale` times to expand it vertically. */
        for (int sy = 0; sy < FB_CONSOLE_FONT_SCALE; sy++) {
            uint32_t* dst = buf
                + (size_t)((uint32_t)py + (uint32_t)row * FB_CONSOLE_FONT_SCALE
                           + (uint32_t)sy) * (size_t)wpr
                + (size_t)px;
            fb_copy32(dst, scanline, out_w);
        }
    }

    /* One dirty-bitmap update per glyph instead of one per micro-pixel. */
    fb_mark_dirty_rows((uint32_t)py, (uint32_t)py + out_h);
}

void clear_screen(void) {
    fb_clear(COLOR_BLACK);
    cursor_x = 0;
    cursor_y = 0;
}

void scroll(void) {
    uint32_t w = fb_width;
    uint32_t h = fb_height;
    /* Pick the shadow if armed: an FB->FB copy on WC memory pays the
     * uncached-read penalty for every source byte. Shadow->shadow runs
     * out of L1/L2 at cache speed and only the final flush hits MMIO. */
    uint32_t* buf = fb_render_buf();
    if (!buf || w == 0 || h == 0)
        return;

    uint32_t wpr = fb_pitch / 4u;
    if (wpr == 0)
        return;

    uint32_t shift = FB_CONSOLE_CHAR_HEIGHT;
    if (shift >= h)
        return;

    /* One bulk REP MOVSD over (h - shift) full scanlines worth of words,
     * instead of (h - shift) separate byte-wise memcpy() calls. Forward
     * copy is correct because dst (row 0) is strictly below src (row
     * shift) — the standard REP MOVSD overlap rule for ascending copy. */
    uint32_t words = (h - shift) * wpr;
    fb_copy32(buf, buf + (size_t)shift * (size_t)wpr, words);

    /* Black out the freshly exposed bottom band. */
    fb_fill_rect(0, h - FB_CONSOLE_CHAR_HEIGHT, w, FB_CONSOLE_CHAR_HEIGHT, COLOR_BLACK);

    /* Every scanline of the screen changed (the shift moved rows 0..h-shift,
     * the fill_rect wrote rows h-shift..h). Single envelope update. */
    fb_mark_dirty_rows(0, h);

    cursor_y -= (int)FB_CONSOLE_CHAR_HEIGHT;
    if (cursor_y < 0)
        cursor_y = 0;
}

void kprint_color(char* message, uint32_t color) {
    vmm_sync_kernel_mmio_mappings(get_current_pd());

    uint32_t w = fb_get_width();
    uint32_t h = fb_get_height();
    int have_fb = (w != 0 && h != 0);

    for (int i = 0; message[i] != '\0'; i++) {
        char c = message[i];
        serial_putc(c);
        if (!have_fb)
            continue;

        if (c == '\n') {
            cursor_x = 0;
            cursor_y += FB_CONSOLE_CHAR_HEIGHT;
        } else if (c == '\r') {
            cursor_x = 0;
        } else if (c == '\t') {
            int tab_w = FB_CONSOLE_CHAR_WIDTH * 4;
            cursor_x = (cursor_x / tab_w + 1) * tab_w;
        } else {
            fb_draw_char_scaled(c, cursor_x, cursor_y, color);
            cursor_x += FB_CONSOLE_CHAR_WIDTH;
        }

        if (cursor_x + FB_CONSOLE_CHAR_WIDTH > (int)w) {
            cursor_x = 0;
            cursor_y += FB_CONSOLE_CHAR_HEIGHT;
        }

        if (cursor_y + FB_CONSOLE_CHAR_HEIGHT > (int)h) {
            scroll();
        }
    }

    /* Ship the accumulated changes to the real framebuffer in one pass.
     * No-op when the shadow is not armed. */
    fb_flush();
}

void kprint(char* message) {
    kprint_color(message, COLOR_WHITE);
}

void kprint_at(char* message, int x, int y) {
    cursor_x = x;
    cursor_y = y;
    kprint(message);
}

void init_framebuffer(void) {
    fb_init_result_t status = fb_get_init_status();

    if (status != FB_INIT_OK) {
        static const char* fb_errors[] = {
            [FB_INIT_NO_FLAG]    = "multiboot2 framebuffer tag missing",
            [FB_INIT_HIGH_ADDR]  = "framebuffer address above 4 GB (not mappable)",
            [FB_INIT_BAD_TYPE]   = "framebuffer type != 1 (not RGB direct-color)",
            [FB_INIT_BAD_BPP]    = "bpp != 32 (only 32-bit color supported)",
            [FB_INIT_NULL_PARAM] = "null address or zero width/height",
        };
        klog(LOG_ERROR, fb_errors[status]);
        klog(LOG_FAIL,  "Framebuffer — cannot continue without display");
        return;
    }

}

int get_cursor_x(void) {
    return cursor_x;
}
int get_cursor_y(void) {
    return cursor_y;
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
    if (!fb_buffer || x >= fb_width || y >= fb_height)
        return;

    uint32_t wpr = fb_pitch / 4u;
    if (wpr == 0)
        return;

    uint64_t offset = (uint64_t)y * (uint64_t)wpr + (uint64_t)x;
    uint64_t max_words = (uint64_t)fb_height * (uint64_t)wpr;
    if (offset >= max_words)
        return;

    fb_render_buf()[(size_t)offset] = color;
    fb_mark_dirty_row(y);
}

void fb_fill_rect(uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint32_t color) {
    if (x >= fb_width || y >= fb_height)
        return;
    if (x + width > fb_width)
        width = fb_width - x;
    if (y + height > fb_height)
        height = fb_height - y;

    uint32_t words_per_row = fb_pitch / 4u;
    if (words_per_row == 0)
        return;

    uint32_t* buf = fb_render_buf();
    for (uint32_t row = 0; row < height; row++) {
        uint64_t off = (uint64_t)(y + row) * (uint64_t)words_per_row + (uint64_t)x;
        uint64_t maxw = (uint64_t)fb_height * (uint64_t)words_per_row;
        if (off >= maxw)
            return;
        uint32_t* line = buf + (size_t)off;
        for (uint32_t col = 0; col < width; col++)
            line[col] = color;
    }
    fb_mark_dirty_rows(y, y + height);
}

void fb_clear(uint32_t color) {
    if (!fb_buffer || fb_width == 0 || fb_height == 0)
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
    size_t shadow_bytes = (size_t)fb_pitch * (size_t)fb_height;
    uint32_t* shadow = (uint32_t*)kmalloc_aligned((uint32_t)shadow_bytes, 4096);
    if (!shadow) {
        klog(LOG_WARN, "FB shadow: kmalloc_aligned failed; staying in direct mode");
        return;
    }

    uint8_t* dirty = (uint8_t*)kmalloc(fb_height);
    if (!dirty) {
        kfree_aligned(shadow);
        klog(LOG_WARN, "FB shadow: dirty bitmap alloc failed");
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

}

void fb_flush(void) {
    if (!fb_shadow_armed)                     return;
    if (!fb_buffer || !fb_shadow || !fb_dirty_row) return;
    if (fb_dirty_y_min > fb_dirty_y_max)      return;   /* nothing dirty */

    uint32_t wpr = fb_pitch / 4u;
    if (wpr == 0) return;

    uint32_t y_lo = fb_dirty_y_min;
    uint32_t y_hi = fb_dirty_y_max;
    if (y_hi >= fb_height) y_hi = fb_height - 1;

    /* Coalesce consecutive dirty scanlines into single REP MOVSD bursts.
     * Worst case (every other row dirty) degenerates to one MOVSD per
     * row; best case (e.g. after scroll() marked the whole screen) becomes
     * one giant blit straight into the WC framebuffer — exactly the
     * write pattern WC was designed to coalesce on the bus. */
    uint32_t y = y_lo;
    while (y <= y_hi) {
        if (!fb_dirty_row[y]) { y++; continue; }
        uint32_t run_start = y;
        while (y <= y_hi && fb_dirty_row[y]) {
            fb_dirty_row[y] = 0;
            y++;
        }
        uint32_t run_len = y - run_start;
        uint32_t* dst = fb_buffer + (size_t)run_start * (size_t)wpr;
        uint32_t* src = fb_shadow + (size_t)run_start * (size_t)wpr;
        /* `wpr` words per row * run_len rows. Even if pitch has stride
         * padding, copying it through is harmless — those bytes aren't
         * displayed and the source is just shadow garbage anyway. */
        fb_copy32(dst, src, run_len * wpr);
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
