#include "font.h"

#define PSF2_MAGIC0 0x72u
#define PSF2_MAGIC1 0xB5u
#define PSF2_MAGIC2 0x4Au
#define PSF2_MAGIC3 0x86u
#define PSF2_MAXVERSION         0u
#define PSF2_HEADER_SIZE        32u
#define PSF2_HAS_UNICODE_TABLE  0x00000001u

#define PSF2_SEPARATOR  0xFFu
#define PSF2_STARTSEQ   0xFEu

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static int utf8_len(uint8_t b) {
    if (b < 0x80u) return 1;
    if (b < 0xC0u) return 0;
    if (b < 0xE0u) return 2;
    if (b < 0xF0u) return 3;
    if (b < 0xF8u) return 4;
    return 0;
}

static int utf8_decode(const uint8_t *s, uint32_t avail, uint32_t *cp) {
    if (avail == 0) return 0;
    uint8_t b0 = s[0];
    int n = utf8_len(b0);
    if (n == 0 || (uint32_t)n > avail) return 0;

    uint32_t v;
    if (n == 1) {
        v = b0;
    } else {
        v = (n == 2) ? (uint32_t)(b0 & 0x1Fu)
          : (n == 3) ? (uint32_t)(b0 & 0x0Fu)
                     : (uint32_t)(b0 & 0x07u);
        for (int i = 1; i < n; i++) {
            uint8_t b = s[i];
            if ((b & 0xC0u) != 0x80u) return 0;
            v = (v << 6) | (uint32_t)(b & 0x3Fu);
        }
        if (n == 2 && v < 0x80u)      return 0;
        if (n == 3 && v < 0x800u)     return 0;
        if (n == 4 && v < 0x10000u)   return 0;
        if (v > 0x10FFFFu)            return 0;
    }
    *cp = v;
    return n;
}

static int fill_unicode_map(console_font_t *f) {
    const uint8_t *p  = f->blob + f->unicodes_off;
    const uint8_t *end = f->blob + f->blob_size;

    for (uint32_t g = 0; g < f->num_glyphs; g++) {
        for (;;) {
            if (p >= end) return FONT_ERR_UNICODE;
            uint8_t b = *p;

            if (b == PSF2_SEPARATOR) {
                p++;
                break;
            }
            if (b == PSF2_STARTSEQ) {
                p++;
                while (p < end && *p != PSF2_STARTSEQ && *p != PSF2_SEPARATOR) {
                    uint32_t cp;
                    int n = utf8_decode(p, (uint32_t)(end - p), &cp);
                    if (n == 0) return FONT_ERR_UNICODE;
                    p += (uint32_t)n;
                }
                continue;
            }

            uint32_t cp;
            int n = utf8_decode(p, (uint32_t)(end - p), &cp);
            if (n == 0) return FONT_ERR_UNICODE;
            if (cp <= 0xFFu)
                f->glyph_of[cp] = (uint16_t)g;
            p += (uint32_t)n;
        }
    }
    return FONT_OK;
}

int font_parse_psf2(const uint8_t *blob, uint32_t blob_size,
                    console_font_t *out) {
    if (!blob || !out || blob_size < PSF2_HEADER_SIZE)
        return FONT_ERR_BAD_SIZE;

    if (blob[0] != PSF2_MAGIC0 || blob[1] != PSF2_MAGIC1 ||
        blob[2] != PSF2_MAGIC2 || blob[3] != PSF2_MAGIC3)
        return FONT_ERR_NOT_PSF2;

    uint32_t version       = rd32(blob + 4);
    uint32_t headersize    = rd32(blob + 8);
    uint32_t flags         = rd32(blob + 12);
    uint32_t numglyph      = rd32(blob + 16);
    uint32_t bytesperglyph = rd32(blob + 20);
    uint32_t height        = rd32(blob + 24);
    uint32_t width         = rd32(blob + 28);

    if (version != PSF2_MAXVERSION)                     return FONT_ERR_BAD_VERSION;
    if (headersize < PSF2_HEADER_SIZE ||
        headersize > blob_size)                         return FONT_ERR_BAD_HEADER;
    if (width == 0 || width > CONSOLE_FONT_MAX_WIDTH)   return FONT_ERR_BAD_HEADER;
    if (height == 0 || height > CONSOLE_FONT_MAX_HEIGHT) return FONT_ERR_BAD_HEADER;
    if (numglyph == 0 || numglyph > CONSOLE_FONT_MAX_GLYPHS)
                                                        return FONT_ERR_BAD_HEADER;

    uint32_t bpr = (width + 7u) / 8u;
    if (bytesperglyph != height * bpr)                  return FONT_ERR_BAD_HEADER;
    if ((blob_size - headersize) < numglyph * bytesperglyph)
                                                        return FONT_ERR_GLYPHS;

    console_font_t f;
    f.width           = width;
    f.height          = height;
    f.bytes_per_glyph = bytesperglyph;
    f.bytes_per_row   = bpr;
    f.num_glyphs      = numglyph;
    f.glyphs_off      = headersize;
    f.blob            = blob;
    f.blob_size       = blob_size;
    for (uint32_t i = 0; i < 256u; i++)
        f.glyph_of[i] = CONSOLE_FONT_NO_GLYPH;

    if (flags & PSF2_HAS_UNICODE_TABLE) {
        f.unicodes_off = headersize + numglyph * bytesperglyph;
        if (f.unicodes_off >= blob_size) return FONT_ERR_UNICODE;
        int rc = fill_unicode_map(&f);
        if (rc != FONT_OK) return rc;
    } else {
        f.unicodes_off = 0;
        uint32_t m = numglyph < 256u ? numglyph : 256u;
        for (uint32_t i = 0; i < m; i++)
            f.glyph_of[i] = (uint16_t)i;
    }

    *out = f;
    return FONT_OK;
}

int font_glyph_index(const console_font_t *f, uint32_t codepoint) {
    if (!f) return -1;
    if (codepoint < 256u) {
        uint16_t g = f->glyph_of[codepoint];
        if (g != CONSOLE_FONT_NO_GLYPH && (uint32_t)g < f->num_glyphs)
            return (int)g;
        return -1;
    }
    if (codepoint < f->num_glyphs)
        return (int)codepoint;
    return -1;
}

const uint8_t *font_glyph_bits(const console_font_t *f, uint32_t glyph) {
    if (!f || glyph >= f->num_glyphs) return NULL;
    return f->blob + f->glyphs_off + glyph * f->bytes_per_glyph;
}
