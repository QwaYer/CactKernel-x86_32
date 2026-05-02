#ifndef FB_H
#define FB_H

#include <stdint.h>
#include "kernel.h"    // multiboot_info_t

// Return codes for fb_init()
typedef enum {
    FB_INIT_OK         = 0,
    FB_INIT_NO_FLAG    = 1,   // multiboot2 framebuffer tag missing
    FB_INIT_HIGH_ADDR  = 2,   // address above 4 GiB — not mappable on i386
    FB_INIT_BAD_TYPE   = 3,   // not direct-colour (type != 1)
    FB_INIT_BAD_BPP    = 4,   // not 32bpp
    FB_INIT_NULL_PARAM = 5,   // zero address or zero dimensions
} fb_init_result_t;

// Initialise the framebuffer from a multiboot2 info structure
fb_init_result_t fb_init(multiboot_info_t* mbi);

// Return the result code of the last fb_init() call
fb_init_result_t fb_get_init_status(void);

// Pixel operations (bounds-checked)
void fb_put_pixel(uint32_t x, uint32_t y, uint32_t color);
void fb_fill_rect(uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint32_t color);
void fb_clear(uint32_t color);

// Accessors
uint32_t  fb_get_width(void);
uint32_t  fb_get_height(void);
uint32_t  fb_get_pitch(void);
uint32_t* fb_get_buffer(void);

#endif