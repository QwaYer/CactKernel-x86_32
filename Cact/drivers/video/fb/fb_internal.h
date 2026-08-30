#ifndef FB_INTERNAL_H
#define FB_INTERNAL_H

#include "fb.h"

#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

/* Console state (fb_text.c). */
extern int cursor_x;
extern int cursor_y;
extern uint32_t current_fb_color;

/* Framebuffer state (fb.c). */
extern uint32_t* fb_buffer;
extern uint32_t  fb_width;
extern uint32_t  fb_height;
extern uint32_t  fb_pitch;
extern uint8_t   fb_bpp;

/* Shadow buffer state (fb.c). */
extern uint32_t* fb_shadow;
extern uint8_t*  fb_dirty_row;
extern uint32_t  fb_dirty_y_min;
extern uint32_t  fb_dirty_y_max;
extern int       fb_shadow_armed;

/* Drawing-path helpers (fb.c). */
uint32_t* fb_render_buf(void);
void fb_mark_dirty_row(uint32_t y);
void fb_mark_dirty_rows(uint32_t y0, uint32_t y1);

/* Fast 32-bit-aligned forward copy via REP MOVSD with prefetch. */
void fb_copy32(uint32_t* dst, const uint32_t* src, uint32_t n_words);

/* Glyph rasteriser (fb_text.c). */
void fb_draw_char_scaled(char c, int px, int py, uint32_t color);

#endif
