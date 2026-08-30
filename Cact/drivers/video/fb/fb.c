#include "fb.h"
#include "klib.h"
#include "memory.h"
#include "serial.h"
#include "kernel.h"
#include <stddef.h>

static int cursor_x = 0;
static int cursor_y = 0;

static uint32_t current_fb_color = COLOR_WHITE;

static uint32_t*        fb_buffer      = 0;
static uint32_t         fb_width       = 0;
static uint32_t         fb_height      = 0;
static uint32_t         fb_pitch       = 0;
static uint8_t          fb_bpp         = 0;
static fb_init_result_t fb_last_status = FB_INIT_OK;

#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

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
    return likely(fb_shadow_armed) ? fb_shadow : fb_buffer;
}

static inline void fb_mark_dirty_row(uint32_t y) {
    if (unlikely(!fb_shadow_armed)) return;
    if (unlikely(y >= fb_height))   return;
    fb_dirty_row[y] = 1;
    if (y < fb_dirty_y_min) fb_dirty_y_min = y;
    if (y > fb_dirty_y_max) fb_dirty_y_max = y;
}

static inline void fb_mark_dirty_rows(uint32_t y0, uint32_t y1) {
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

/* Fast 32-bit-aligned forward copy via REP MOVSD with prefetch.
 * Prefetch the first cache line of source so the CPU can start the
 * read transaction while the rep movsl microcode is still warming up.
 * Safe for the dst < src overlap case used by scroll(). */
static inline void fb_copy32(uint32_t* dst, const uint32_t* src, uint32_t n_words) {
    __builtin_prefetch(src, 0, 3);
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
 * each of which re-did the bounds check and the dirty-bitmap update.
 *
 * The scanline builder is fully unrolled — all loop counters are
 * compile-time constants — so GCC emits straight-line code with zero
 * branch mispredicts for the pixel-expansion phase. */
static void fb_draw_char_scaled(char c, int px, int py, uint32_t color) {
    if ((unsigned char)c >= 128) return;

    const uint32_t out_w = (uint32_t)FB_CONSOLE_CHAR_WIDTH;
    const uint32_t out_h = (uint32_t)FB_CONSOLE_CHAR_HEIGHT;

    if (unlikely(px < 0 || py < 0))              return;
    if (unlikely((uint32_t)px + out_w > fb_width))  return;
    if (unlikely((uint32_t)py + out_h > fb_height)) return;

    uint32_t wpr = fb_pitch / 4u;
    if (unlikely(wpr == 0)) return;

    uint32_t* buf = fb_render_buf();
    if (unlikely(!buf)) return;

    const uint8_t* glyph = font8x8_basic[(unsigned char)c];

    uint32_t* const base_row = buf + (size_t)py * (size_t)wpr + (size_t)px;
    uint32_t  const row_stride = (uint32_t)FB_CONSOLE_FONT_SCALE * (uint32_t)wpr;

    uint32_t scanline[FB_CONSOLE_CHAR_WIDTH];

    for (int row = 0; row < FONT_HEIGHT; row++) {
        uint8_t bits = glyph[row];

        uint32_t p0  = (bits & (1u << 0)) ? color : COLOR_BLACK;
        uint32_t p1  = (bits & (1u << 1)) ? color : COLOR_BLACK;
        uint32_t p2  = (bits & (1u << 2)) ? color : COLOR_BLACK;
        uint32_t p3  = (bits & (1u << 3)) ? color : COLOR_BLACK;
        uint32_t p4  = (bits & (1u << 4)) ? color : COLOR_BLACK;
        uint32_t p5  = (bits & (1u << 5)) ? color : COLOR_BLACK;
        uint32_t p6  = (bits & (1u << 6)) ? color : COLOR_BLACK;
        uint32_t p7  = (bits & (1u << 7)) ? color : COLOR_BLACK;

        scanline[0]  = scanline[1]  = p0;
        scanline[2]  = scanline[3]  = p1;
        scanline[4]  = scanline[5]  = p2;
        scanline[6]  = scanline[7]  = p3;
        scanline[8]  = scanline[9]  = p4;
        scanline[10] = scanline[11] = p5;
        scanline[12] = scanline[13] = p6;
        scanline[14] = scanline[15] = p7;

        /* Inline REP MOVSD — no prefetch needed, source is stack (L1). */
        uint32_t* dst0 = base_row + (size_t)row * (size_t)row_stride;
        uint32_t* dst1 = dst0 + (size_t)wpr;
        uint32_t  cnt  = out_w;
        uint32_t* s    = scanline;
        __asm__ __volatile__ ("rep movsl"
            : "+D"(dst0), "+S"(s), "+c"(cnt) : : "memory");
        cnt = out_w;  s = scanline;
        __asm__ __volatile__ ("rep movsl"
            : "+D"(dst1), "+S"(s), "+c"(cnt) : : "memory");
    }

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
    uint32_t* buf = fb_render_buf();
    if (unlikely(!buf || w == 0 || h == 0))
        return;

    uint32_t wpr = fb_pitch / 4u;
    if (unlikely(wpr == 0))
        return;

    uint32_t shift = FB_CONSOLE_CHAR_HEIGHT;
    if (unlikely(shift >= h))
        return;

    uint32_t words = (h - shift) * wpr;
    fb_copy32(buf, buf + (size_t)shift * (size_t)wpr, words);

    fb_fill_rect(0, h - FB_CONSOLE_CHAR_HEIGHT, w, FB_CONSOLE_CHAR_HEIGHT, COLOR_BLACK);

    fb_mark_dirty_rows(0, h);

    cursor_y -= (int)FB_CONSOLE_CHAR_HEIGHT;
    if (cursor_y < 0)
        cursor_y = 0;
}

static const uint32_t ansi_colors[16] = {
    COLOR_BLACK, COLOR_RED, COLOR_GREEN, 0xAAAA00,
    COLOR_BLUE, COLOR_MAGENTA, COLOR_CYAN, COLOR_LIGHT_GREY,
    COLOR_DARK_GREY, COLOR_LIGHT_RED, COLOR_LIGHT_GREEN, COLOR_LIGHT_BROWN,
    COLOR_LIGHT_BLUE, COLOR_LIGHT_MAGENTA, COLOR_LIGHT_CYAN, COLOR_WHITE,
};

void printk_color(char* message, uint32_t color) {
    uint32_t w = fb_get_width();
    uint32_t h = fb_get_height();
    int have_fb = (w != 0 && h != 0);

    current_fb_color = color;

    if (unlikely(!have_fb)) {
        for (int i = 0; message[i] != '\0'; i++) {
            char c = message[i];
            serial_putc(c);
        }
        return;
    }

    uint32_t max_x = w - FB_CONSOLE_CHAR_WIDTH;
    uint32_t max_y = h - FB_CONSOLE_CHAR_HEIGHT;

    for (int i = 0; message[i] != '\0'; i++) {
        char c = message[i];

        if (c == '\033') {
            serial_putc(c);
            i++;
            if (!message[i]) break;
            serial_putc(message[i]);
            if (message[i] != '[') continue;
            i++;

            int params[4], np = 0, val = 0, has_val = 0;
            while (message[i]) {
                if (message[i] >= '0' && message[i] <= '9') {
                    val = val * 10 + (message[i] - '0');
                    has_val = 1;
                    i++;
                } else if (message[i] == ';') {
                    if (has_val) params[np++] = val;
                    else params[np++] = 0;
                    val = 0; has_val = 0;
                    serial_putc(';');
                    i++;
                } else {
                    if (has_val) params[np++] = val;
                    serial_putc(message[i]);

                    if (message[i] == 'm') {
                        for (int p = 0; p < np; p++) {
                            if (params[p] >= 30 && params[p] <= 37)
                                current_fb_color = ansi_colors[params[p] - 30];
                            else if (params[p] >= 90 && params[p] <= 97)
                                current_fb_color = ansi_colors[8 + params[p] - 90];
                            else if (params[p] == 0)
                                current_fb_color = color;
                        }
                    } else if (message[i] == 'H') {
                        int row = (np > 0) ? params[0] : 1;
                        int col = (np > 1) ? params[1] : 1;
                        cursor_y = (row - 1) * FB_CONSOLE_CHAR_HEIGHT;
                        cursor_x = (col - 1) * FB_CONSOLE_CHAR_WIDTH;
                        if (cursor_y < 0) cursor_y = 0;
                        if (cursor_x < 0) cursor_x = 0;
                    } else if (message[i] == 'J') {
                        int mode = (np > 0) ? params[0] : 0;
                        if (mode == 2) {
                            clear_screen();
                        } else if (mode == 0) {
                            uint32_t fy = (uint32_t)cursor_y;
                            uint32_t fx = (uint32_t)cursor_x;
                            while ((int)fy <= (int)max_y) {
                                uint32_t sx = (fy == (uint32_t)cursor_y) ? fx : 0;
                                fb_fill_rect(sx, fy, w - sx, FB_CONSOLE_CHAR_HEIGHT, COLOR_BLACK);
                                fy += FB_CONSOLE_CHAR_HEIGHT;
                            }
                            fb_mark_dirty_rows((uint32_t)cursor_y / FB_CONSOLE_CHAR_HEIGHT,
                                               max_y / FB_CONSOLE_CHAR_HEIGHT);
                        }
                    } else if (message[i] == 'K') {
                        int mode = (np > 0) ? params[0] : 0;
                        if (mode == 0) {
                            uint32_t sx = (uint32_t)cursor_x;
                            fb_fill_rect(sx, (uint32_t)cursor_y, w - sx, FB_CONSOLE_CHAR_HEIGHT, COLOR_BLACK);
                            fb_mark_dirty_rows((uint32_t)cursor_y / FB_CONSOLE_CHAR_HEIGHT,
                                               (uint32_t)cursor_y / FB_CONSOLE_CHAR_HEIGHT);
                        }
                    }
                    break;
                }
            }
            continue;
        }

        serial_putc(c);

        if (c == '\n') {
            cursor_x = 0;
            cursor_y += FB_CONSOLE_CHAR_HEIGHT;
        } else if (c == '\r') {
            cursor_x = 0;
        } else if (c == '\t') {
            int tab_w = FB_CONSOLE_CHAR_WIDTH * 4;
            cursor_x = (cursor_x / tab_w + 1) * tab_w;
        } else {
            fb_draw_char_scaled(c, cursor_x, cursor_y, current_fb_color);
            cursor_x += FB_CONSOLE_CHAR_WIDTH;
        }

        if ((uint32_t)cursor_x > max_x) {
            cursor_x = 0;
            cursor_y += FB_CONSOLE_CHAR_HEIGHT;
        }

        if ((uint32_t)cursor_y > max_y) {
            scroll();
        }
    }

    fb_flush();
}

void printk(char* message) {
    printk_color(message, COLOR_WHITE);
}

void printk_at(char* message, int x, int y) {
    cursor_x = x;
    cursor_y = y;
    printk(message);
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
    klog(LOG_OK, "Framebuffer verified for kernel console (post-paging)");
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
        klog(LOG_WARN, "FB shadow: pitch*height overflow; staying in direct mode");
        return;
    }
    size_t shadow_bytes = fb_pitch_s * fb_height_s;
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
    klog(LOG_OK, "Framebuffer shadow buffer armed (WB RAM back-buffer)");
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
