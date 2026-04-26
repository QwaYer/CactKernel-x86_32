#ifndef FB_H
#define FB_H

#include <stdint.h>
#include "kernel.h"

typedef enum {
    FB_INIT_OK         = 0,
    FB_INIT_NO_FLAG    = 1,
    FB_INIT_HIGH_ADDR  = 2,
    FB_INIT_BAD_TYPE   = 3,
    FB_INIT_BAD_BPP    = 4,
    FB_INIT_NULL_PARAM = 5,
} fb_init_result_t;

fb_init_result_t fb_init(multiboot_info_t* mbi);
fb_init_result_t fb_get_init_status(void);

void fb_put_pixel(uint32_t x, uint32_t y, uint32_t color);
void fb_fill_rect(uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint32_t color);
void fb_clear(uint32_t color);

uint32_t fb_get_width();
uint32_t fb_get_height();
uint32_t fb_get_pitch();
uint32_t* fb_get_buffer();

#endif