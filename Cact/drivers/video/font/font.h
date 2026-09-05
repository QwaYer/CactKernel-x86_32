#ifndef FONT_H
#define FONT_H

#include <stdint.h>
#include <stddef.h>

/* Path of the console font inside the boot cctkfs archive (see font_boot.c). */
#define CONSOLE_FONT_CCTKFS_PATH "/lib/consolefont.psf"

/* Parser sanity limits.  The console rasteriser scales glyphs by
 * FB_CONSOLE_FONT_SCALE (2), so a 32x64 font already yields a 64x128 cell —
 * anything larger is unusable by the linear framebuffer console. */
#define CONSOLE_FONT_MAX_WIDTH   32u
#define CONSOLE_FONT_MAX_HEIGHT  64u
#define CONSOLE_FONT_MAX_GLYPHS  4096u

/* Glyph index marker in the 8-bit fast map (glyph indices stay < 4096). */
#define CONSOLE_FONT_NO_GLYPH    0xFFFFu

typedef struct {
    uint32_t          width;            /* glyph width in pixels      */
    uint32_t          height;           /* glyph height in pixels     */
    uint32_t          bytes_per_glyph;  /* row stride between glyphs  */
    uint32_t          bytes_per_row;    /* (width + 7) / 8            */
    uint32_t          num_glyphs;
    uint32_t          glyphs_off;       /* blob offset of the bitmap  */
    uint32_t          unicodes_off;     /* blob offset of unicode map */
    uint32_t          blob_size;
    const uint8_t    *blob;             /* whole PSF2 file (borrowed) */
    /* Byte (0..255) -> glyph index fast map for the console; entries are
     * CONSOLE_FONT_NO_GLYPH when the font has no glyph for that byte. */
    uint16_t          glyph_of[256];
} console_font_t;

enum {
    FONT_OK              = 0,
    FONT_ERR_BAD_SIZE    = -1,
    FONT_ERR_NOT_PSF2    = -2,
    FONT_ERR_BAD_VERSION = -3,
    FONT_ERR_BAD_HEADER  = -4,
    FONT_ERR_GLYPHS      = -5,
    FONT_ERR_UNICODE     = -6,
    FONT_ERR_NOT_FOUND   = -7,
};

/* Parse a PSF2 blob into `out`.  Glyph bitmaps and the unicode table are
 * referenced in place — the caller must keep `blob` alive.  Never
 * allocates, so it is safe before the heap exists.  Returns 0 / FONT_* */
int font_parse_psf2(const uint8_t *blob, uint32_t blob_size,
                    console_font_t *out);

/* Glyph index for a codepoint, or -1 when the font has none. */
int font_glyph_index(const console_font_t *f, uint32_t codepoint);

/* Pointer to the glyph bitmap, or NULL when the glyph index is invalid. */
const uint8_t *font_glyph_bits(const console_font_t *f, uint32_t glyph);

/* Load the boot console font from the cctkfs archive entry `path` and make
 * it the active font.  The font blob stays in the kernel .bss staging area,
 * so no heap is touched.  Returns FONT_OK or a FONT_ERR_* code. */
int font_load_boot(const char *path);

/* Active boot font, or NULL before font_load_boot() succeeds. */
const console_font_t *font_get_active(void);

#endif
