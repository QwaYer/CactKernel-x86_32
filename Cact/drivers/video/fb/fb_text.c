#include "fb.h"
#include "fb_internal.h"
#include "klib.h"
#include "memory.h"
#include "serial.h"
#include "kernel.h"
#include <stddef.h>
#include <stdarg.h>

int cursor_x = 0;
int cursor_y = 0;

uint32_t current_fb_color = COLOR_WHITE;

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
void fb_draw_char_scaled(char c, int px, int py, uint32_t color) {
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

// Linux-style printk: format string with optional KERN_<level> prefix.
// Level prefixes (KERN_SOH + digit) set the console colour.
void printk(const char* fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);

    uint32_t color = COLOR_WHITE;
    if (fmt[0] == '\x01' && fmt[1] >= '0' && fmt[1] <= '7') {
        switch (fmt[1]) {
        case '0': color = COLOR_LIGHT_RED;    break; // KERN_EMERG
        case '1': color = COLOR_LIGHT_RED;    break; // KERN_ALERT
        case '2': color = COLOR_LIGHT_RED;    break; // KERN_CRIT
        case '3': color = COLOR_LIGHT_RED;    break; // KERN_ERR
        case '4': color = COLOR_LIGHT_BROWN;  break; // KERN_WARNING
        case '5': color = COLOR_WHITE;        break; // KERN_NOTICE
        case '6': color = COLOR_LIGHT_GREY;   break; // KERN_INFO
        case '7': color = COLOR_DARK_GREY;    break; // KERN_DEBUG
        }
        fmt += 2;
    }

    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    printk_color(buf, color);
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
        pr_err("%s", fb_errors[status]);
        pr_crit("Framebuffer — cannot continue without display");
        return;
    }
    pr_info("Framebuffer verified for kernel console (post-paging)");
}

int get_cursor_x(void) {
    return cursor_x;
}
int get_cursor_y(void) {
    return cursor_y;
}
