#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "font.h"

static int failures = 0;

#define CHECK(cond, msg)                                             \
    do { if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } }   \
    while (0)

static uint8_t *read_file(const char *path, uint32_t *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = (uint8_t *)malloc((size_t)n);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); fclose(f); return NULL; }
    fclose(f);
    *out_size = (uint32_t)n;
    return buf;
}

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static void test_real_font(const char *path) {
    uint32_t size = 0;
    uint8_t *blob = read_file(path, &size);
    CHECK(blob != NULL, "read generated font");
    if (!blob) return;

    console_font_t f;
    int rc = font_parse_psf2(blob, size, &f);
    CHECK(rc == FONT_OK, "parse generated font");
    if (rc != FONT_OK) { free(blob); return; }

    CHECK(f.width == 8, "width == 8");
    CHECK(f.height == 8, "height == 8");
    CHECK(f.num_glyphs == 256, "num_glyphs == 256");
    CHECK(f.bytes_per_glyph == 8, "bytes_per_glyph == 8");
    CHECK(f.bytes_per_row == 1, "bytes_per_row == 1");
    CHECK(f.glyphs_off == 32, "glyphs_off == 32");
    CHECK(f.unicodes_off == 32u + 256u * 8u, "unicodes_off right after bitmaps");

    for (int cp = 0; cp < 128; cp++)
        CHECK(font_glyph_index(&f, (uint32_t)cp) == cp, "identity map for 0..127");
    for (int cp = 128; cp < 256; cp++)
        CHECK(font_glyph_index(&f, (uint32_t)cp) == -1, "no glyph for 128..255");
    CHECK(font_glyph_index(&f, 0x400) == -1, "no glyph beyond range");

    static const uint8_t a_glyph[8] = { 0x30, 0x78, 0xCC, 0xCC, 0xFC, 0xCC, 0xCC, 0x00 };
    const uint8_t *ga = font_glyph_bits(&f, 0x41);
    CHECK(ga != NULL && memcmp(ga, a_glyph, 8) == 0, "'A' bitmap bit-reversed correctly");

    static const uint8_t bang_glyph[8] = { 0x18, 0x3C, 0x3C, 0x18, 0x18, 0x00, 0x18, 0x00 };
    const uint8_t *gb = font_glyph_bits(&f, 0x21);
    CHECK(gb != NULL && memcmp(gb, bang_glyph, 8) == 0, "'!' bitmap correct");

    CHECK(font_glyph_bits(&f, 256) == NULL, "glyph 256 out of range -> NULL");

    for (uint32_t cut = size - 1; cut >= 32 && cut > size - 8; cut--) {
        int r2 = font_parse_psf2(blob, cut, &f);
        CHECK(r2 != FONT_OK, "truncated font rejected");
    }
    free(blob);
}

static void test_synthetic_unicode_table(void) {
    enum { NG = 3, W = 8, H = 8 };
    uint32_t bpg = H * ((W + 7) / 8);
    uint8_t buf[32 + NG * bpg + 64];
    memset(buf, 0, sizeof(buf));

    buf[0] = 0x72; buf[1] = 0xB5; buf[2] = 0x4A; buf[3] = 0x86;
    put32(buf + 4, 0);            /* version */
    put32(buf + 8, 32);           /* header size */
    put32(buf + 12, 1);           /* flags: HAS_UNICODE_TABLE */
    put32(buf + 16, NG);
    put32(buf + 20, bpg);
    put32(buf + 24, H);
    put32(buf + 28, W);

    for (int g = 0; g < NG; g++)               /* distinct glyph pattern per index */
        memset(buf + 32 + (size_t)g * bpg, (uint8_t)(0x10 + g), bpg);

    uint8_t *u = buf + 32 + NG * bpg;
    uint8_t *p = u;
    *p++ = 'A'; *p++ = 'B'; *p++ = 0xFF;                 /* glyph 0: A, B */
    *p++ = 0xFE; *p++ = 'C'; *p++ = 0xCC; *p++ = 0x81;   /* glyph 1: seq C + U+0301 */
    *p++ = 0xFF;
    *p++ = 'A'; *p++ = 0xFE; *p++ = 'D'; *p++ = 'E';     /* glyph 2: A + seq D E */
    *p++ = 0xFF;
    uint32_t total = (uint32_t)(p - buf);

    console_font_t f;
    CHECK(font_parse_psf2(buf, total, &f) == FONT_OK, "synthetic font parses");
    CHECK(font_glyph_index(&f, 'A') == 2, "'A' maps to last glyph (2)");
    CHECK(font_glyph_index(&f, 'B') == 0, "'B' maps to glyph 0");
    CHECK(font_glyph_index(&f, 'C') == -1, "sequence member 'C' not mapped");
    CHECK(font_glyph_index(&f, 'D') == -1, "sequence member 'D' not mapped");
    CHECK(font_glyph_index(&f, 'E') == -1, "sequence member 'E' not mapped");
    CHECK(font_glyph_index(&f, 0x301) == -1, "combining mark not mapped as single");
}

static void test_identity_without_table(void) {
    enum { NG = 4, W = 8, H = 8 };
    uint32_t bpg = H;
    uint8_t buf[32 + NG * bpg];
    memset(buf, 0, sizeof(buf));
    buf[0] = 0x72; buf[1] = 0xB5; buf[2] = 0x4A; buf[3] = 0x86;
    put32(buf + 4, 0);
    put32(buf + 8, 32);
    put32(buf + 12, 0);            /* no unicode table */
    put32(buf + 16, NG);
    put32(buf + 20, bpg);
    put32(buf + 24, H);
    put32(buf + 28, W);

    console_font_t f;
    CHECK(font_parse_psf2(buf, sizeof(buf), &f) == FONT_OK, "no-table font parses");
    for (int cp = 0; cp < NG; cp++)
        CHECK(font_glyph_index(&f, (uint32_t)cp) == cp, "identity fallback without table");
    CHECK(font_glyph_index(&f, NG) == -1, "no identity beyond glyphs");
}

static void test_bad_inputs(void) {
    uint8_t junk[64];
    memset(junk, 0xAA, sizeof(junk));
    console_font_t f;
    CHECK(font_parse_psf2(junk, 4, &f) == FONT_ERR_BAD_SIZE, "tiny buffer rejected");
    CHECK(font_parse_psf2(NULL, 64, &f) == FONT_ERR_BAD_SIZE, "NULL blob rejected");

    uint8_t bad[64];
    memset(bad, 0, sizeof(bad));
    bad[0] = 0x72; bad[1] = 0xB5; bad[2] = 0x4A; bad[3] = 0x87;  /* wrong magic */
    put32(bad + 4, 0);
    put32(bad + 8, 32);
    put32(bad + 12, 0);
    put32(bad + 16, 2);
    put32(bad + 20, 8);
    put32(bad + 24, 8);
    put32(bad + 28, 8);
    CHECK(font_parse_psf2(bad, sizeof(bad), &f) == FONT_ERR_NOT_PSF2, "bad magic rejected");

    uint8_t big[64];
    memset(big, 0, sizeof(big));
    big[0] = 0x72; big[1] = 0xB5; big[2] = 0x4A; big[3] = 0x86;
    put32(big + 4, 0);
    put32(big + 8, 32);
    put32(big + 12, 0);
    put32(big + 16, 100000u);      /* numglyph far beyond blob */
    put32(big + 20, 8);
    put32(big + 24, 8);
    put32(big + 28, 8);
    int rc = font_parse_psf2(big, sizeof(big), &f);
    CHECK(rc == FONT_ERR_BAD_HEADER || rc == FONT_ERR_GLYPHS, "oversized glyph count rejected");
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("usage: %s <consolefont.psf>\n", argv[0]);
        return 2;
    }
    test_real_font(argv[1]);
    test_synthetic_unicode_table();
    test_identity_without_table();
    test_bad_inputs();

    if (failures) {
        printf("%d FAILURES\n", failures);
        return 1;
    }
    printf("all font parser tests passed\n");
    return 0;
}
