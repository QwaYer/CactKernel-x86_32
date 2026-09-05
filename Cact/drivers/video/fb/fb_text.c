#include "fb.h"
#include "fb_internal.h"
#include "font.h"
#include "klib.h"
#include "memory.h"
#include "serial.h"
#include "kernel.h"
#include "klog.h"
#include <stddef.h>
#include <stdarg.h>

int cursor_x = 0;
int cursor_y = 0;

uint32_t current_fb_color = COLOR_WHITE;

uint32_t fb_char_cell_w(void) {
    const console_font_t *f = font_get_active();
    return (f ? f->width : 8u) * (uint32_t)FB_CONSOLE_FONT_SCALE;
}

uint32_t fb_char_cell_h(void) {
    const console_font_t *f = font_get_active();
    return (f ? f->height : 8u) * (uint32_t)FB_CONSOLE_FONT_SCALE;
}

/* Rasterise one glyph from the active PSF2 font at (px, py) into the shadow
 * (or, when the shadow is inactive, the real framebuffer).  Each glyph row is
 * expanded `scale` times horizontally and stamped down `scale` times. */
void fb_draw_char_scaled(char c, int px, int py, uint32_t color) {
    const console_font_t *f = font_get_active();
    if (unlikely(!f)) return;

    const uint32_t scale = (uint32_t)FB_CONSOLE_FONT_SCALE;
    const uint32_t fw    = f->width;
    const uint32_t fh    = f->height;
    const uint32_t out_w = fw * scale;
    const uint32_t out_h = fh * scale;

    if (unlikely(px < 0 || py < 0))              return;
    if (unlikely((uint32_t)px + out_w > fb_width))  return;
    if (unlikely((uint32_t)py + out_h > fb_height)) return;

    int gi = font_glyph_index(f, (uint32_t)(uint8_t)c);
    if (unlikely(gi < 0)) return;

    const uint8_t *glyph = font_glyph_bits(f, (uint32_t)gi);
    if (unlikely(!glyph)) return;

    uint32_t wpr = fb_pitch / 4u;
    if (unlikely(wpr == 0)) return;

    uint32_t* buf = fb_render_buf();
    if (unlikely(!buf)) return;

    uint32_t scanline[CONSOLE_FONT_MAX_WIDTH * FB_CONSOLE_FONT_SCALE];

    uint32_t* const base_row = buf + (size_t)py * (size_t)wpr + (size_t)px;

    for (uint32_t row = 0; row < fh; row++) {
        const uint8_t *rb = glyph + (size_t)row * (size_t)f->bytes_per_row;

        for (uint32_t col = 0; col < fw; col++) {
            uint8_t  mask = (uint8_t)(0x80u >> (col & 7u));
            uint32_t v    = (rb[col >> 3] & mask) ? color : COLOR_BLACK;
            uint32_t *sp  = &scanline[col * scale];
            for (uint32_t k = 0; k < scale; k++)
                sp[k] = v;
        }

        for (uint32_t s = 0; s < scale; s++) {
            uint32_t* dst = base_row + (size_t)(row * scale + s) * (size_t)wpr;
            uint32_t  cnt = out_w;
            uint32_t* src = scanline;
            __asm__ __volatile__ ("rep movsl"
                : "+D"(dst), "+S"(src), "+c"(cnt) : : "memory");
        }
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

    uint32_t shift = fb_char_cell_h();
    if (unlikely(shift >= h))
        return;

    uint32_t words = (h - shift) * wpr;
    fb_copy32(buf, buf + (size_t)shift * (size_t)wpr, words);

    fb_fill_rect(0, h - shift, w, shift, COLOR_BLACK);

    fb_mark_dirty_rows(0, h);

    cursor_y -= (int)shift;
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

    /* Mirror every console line into the kernel message log (/dev/kmsg). */
    klog_feed(message, strlen(message));

    if (unlikely(!have_fb)) {
        for (int i = 0; message[i] != '\0'; i++) {
            char c = message[i];
            serial_putc(c);
        }
        return;
    }

    uint32_t cellw = fb_char_cell_w();
    uint32_t cellh = fb_char_cell_h();
    if (unlikely(cellw == 0 || cellh == 0))
        return;

    uint32_t max_x = w - cellw;
    uint32_t max_y = h - cellh;

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
                        cursor_y = (row - 1) * (int)cellh;
                        cursor_x = (col - 1) * (int)cellw;
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
                                fb_fill_rect(sx, fy, w - sx, cellh, COLOR_BLACK);
                                fy += cellh;
                            }
                            fb_mark_dirty_rows((uint32_t)cursor_y / cellh,
                                               max_y / cellh);
                        }
                    } else if (message[i] == 'K') {
                        int mode = (np > 0) ? params[0] : 0;
                        if (mode == 0) {
                            uint32_t sx = (uint32_t)cursor_x;
                            fb_fill_rect(sx, (uint32_t)cursor_y, w - sx, cellh, COLOR_BLACK);
                            fb_mark_dirty_rows((uint32_t)cursor_y / cellh,
                                               (uint32_t)cursor_y / cellh);
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
            cursor_y += (int)cellh;
        } else if (c == '\r') {
            cursor_x = 0;
        } else if (c == '\t') {
            int tab_w = (int)cellw * 4;
            cursor_x = (cursor_x / tab_w + 1) * tab_w;
        } else {
            fb_draw_char_scaled(c, cursor_x, cursor_y, current_fb_color);
            cursor_x += (int)cellw;
        }

        if ((uint32_t)cursor_x > max_x) {
            cursor_x = 0;
            cursor_y += (int)cellh;
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
        pr_err("  %-11s : %s\n", "framebuffer", fb_errors[status]);
        pr_crit("  %-11s : cannot continue without display\n", "framebuffer");
        return;
    }
    /* Success line (geometry / PAT / shadow) is printed once by kernel.c. */
}

int get_cursor_x(void) {
    return cursor_x;
}
int get_cursor_y(void) {
    return cursor_y;
}
