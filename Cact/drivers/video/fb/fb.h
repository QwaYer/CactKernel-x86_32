#ifndef FB_H
#define FB_H

#include <stdint.h>
#include "kernel.h"    // multiboot_info_t, colors
#include "font.h"

#define FB_CONSOLE_FONT_SCALE  2
#define FB_CONSOLE_CHAR_WIDTH  (FONT_WIDTH * FB_CONSOLE_FONT_SCALE)
#define FB_CONSOLE_CHAR_HEIGHT (FONT_HEIGHT * FB_CONSOLE_FONT_SCALE)

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

/* Post-paging verification / diagnostics (calls kprint/klog). */
void init_framebuffer(void);

/*
 * Optional shadow (back) buffer in regular WB RAM.
 *
 * Once enabled, every drawing primitive (fb_put_pixel, fb_fill_rect, fb_clear,
 * the console glyph rasteriser, scroll()) writes into a kernel-heap copy of
 * the framebuffer instead of touching MMIO directly. Touched rows are tracked
 * in a per-row dirty bitmap; fb_flush() copies only those rows out to the
 * physical framebuffer in one bulk burst.
 *
 * Win vs. plain WC framebuffer:
 *   - scroll() and any FB->FB blit no longer reads from WC memory (which is
 *     uncached and slow); the read side now hits L1/L2 at WB speeds.
 *   - Multiple writes to the same pixel coalesce in the shadow at cache speed
 *     and only the final value is shipped out to the bus.
 *
 * Call AFTER init_heap() and AFTER mtrr_enable_wc_for_framebuffer() so the
 * shadow can be seeded from the current FB contents under WC reads (UC reads
 * during seeding would otherwise stall for tens of ms).
 *
 * If allocation fails, drawing silently stays in direct (non-shadow) mode.
 */
void fb_enable_shadow(void);

/*
 * Flush the dirty range of the shadow buffer to the real framebuffer.
 * No-op when the shadow is not armed. Idempotent. Called automatically at
 * the end of every kprint_color(); drivers that draw outside the console
 * path (cursor overlays, splash screens, …) should call it themselves.
 */
void fb_flush(void);

#endif