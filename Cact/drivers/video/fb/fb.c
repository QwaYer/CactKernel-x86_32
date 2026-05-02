#include "fb.h"

static int cursor_x = 0;
static int cursor_y = 0;

static void fb_draw_char_scaled(char c, int px, int py, uint32_t color) {
    if ((unsigned char)c >= 128) return;
    const uint8_t* glyph = font8x8_basic[(unsigned char)c];
    for (int row = 0; row < FONT_HEIGHT; row++) {
        for (int col = 0; col < FONT_WIDTH; col++) {
            uint32_t pix = (glyph[row] & (1 << col)) ? color : COLOR_BLACK;
            for (int sy = 0; sy < FB_CONSOLE_FONT_SCALE; sy++)
                for (int sx = 0; sx < FB_CONSOLE_FONT_SCALE; sx++)
                    fb_put_pixel(px + col * FB_CONSOLE_FONT_SCALE + sx,
                                 py + row * FB_CONSOLE_FONT_SCALE + sy, pix);
        }
    }
}

void clear_screen(void) {
    fb_clear(COLOR_BLACK);
    cursor_x = 0;
    cursor_y = 0;
}

void scroll(void) {
    uint32_t w = fb_get_width();
    uint32_t h = fb_get_height();
    uint32_t pitch = fb_get_pitch();
    uint32_t* buf = fb_get_buffer();
    if (!buf || w == 0 || h == 0) return;

    uint32_t words_per_row = pitch / 4;
    for (uint32_t y = 0; y + FB_CONSOLE_CHAR_HEIGHT < h; y++)
        for (uint32_t x = 0; x < w; x++)
            buf[y * words_per_row + x] = buf[(y + FB_CONSOLE_CHAR_HEIGHT) * words_per_row + x];

    fb_fill_rect(0, h - FB_CONSOLE_CHAR_HEIGHT, w, FB_CONSOLE_CHAR_HEIGHT, COLOR_BLACK);

    cursor_y -= FB_CONSOLE_CHAR_HEIGHT;
    if (cursor_y < 0) cursor_y = 0;
}

void kprint_color(char* message, uint32_t color) {
    uint32_t w = fb_get_width();
    uint32_t h = fb_get_height();
    if (w == 0 || h == 0) return;

    for (int i = 0; message[i] != '\0'; i++) {
        char c = message[i];
        if (c == '\n') {
            cursor_x = 0;
            cursor_y += FB_CONSOLE_CHAR_HEIGHT;
        } else if (c == '\r') {
            cursor_x = 0;
        } else if (c == '\t') {
            int tab_w = FB_CONSOLE_CHAR_WIDTH * 4;
            cursor_x = (cursor_x / tab_w + 1) * tab_w;
        } else {
            fb_draw_char_scaled(c, cursor_x, cursor_y, color);
            cursor_x += FB_CONSOLE_CHAR_WIDTH;
        }

        if (cursor_x + FB_CONSOLE_CHAR_WIDTH > (int)w) {
            cursor_x = 0;
            cursor_y += FB_CONSOLE_CHAR_HEIGHT;
        }

        if (cursor_y + FB_CONSOLE_CHAR_HEIGHT > (int)h) {
            scroll();
        }
    }
}

void kprint(char* message) {
    kprint_color(message, COLOR_WHITE);
}

void kprint_at(char* message, int x, int y) {
    cursor_x = x;
    cursor_y = y;
    kprint(message);
}

void init_framebuffer(void) {
    kprint("[FB] checking multiboot framebuffer parameters\n");
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

    char buf[16];
    kprint("[FB] addr=0x");
    hex_to_ascii((uint32_t)fb_get_buffer(), buf); kprint(buf);
    kprint("  res=");
    itoa((int)fb_get_width(), buf);  kprint(buf); kprint("x");
    itoa((int)fb_get_height(), buf); kprint(buf);
    kprint("  32bpp  pitch=");
    itoa((int)fb_get_pitch(), buf); kprint(buf); kprint("\n");
    kprint("[FB] cols="); itoa((int)(fb_get_width()  / (8 * FB_CONSOLE_FONT_SCALE)), buf); kprint(buf);
    kprint("  rows=");   itoa((int)(fb_get_height() / (8 * FB_CONSOLE_FONT_SCALE)), buf); kprint(buf); kprint("\n");
    klog(LOG_OK, "Framebuffer ready");
}

int get_cursor_x(void) { return cursor_x; }
int get_cursor_y(void) { return cursor_y; }

// Framebuffer state
static uint32_t*       fb_buffer      = 0;
static uint32_t        fb_width       = 0;
static uint32_t        fb_height      = 0;
static uint32_t        fb_pitch       = 0;    // bytes per scanline
static uint8_t         fb_bpp         = 0;
static fb_init_result_t fb_last_status = FB_INIT_OK;

// Initialise the framebuffer from a multiboot2 framebuffer tag.
// Accepts only 32bpp direct-colour (type=1) with a 32-bit physical address.
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

// Return the result code of the last fb_init() call
fb_init_result_t fb_get_init_status(void) {
    return fb_last_status;
}

// Set a single pixel to the given colour (bounds-checked)
void fb_put_pixel(uint32_t x, uint32_t y, uint32_t color) {
    if (!fb_buffer || x >= fb_width || y >= fb_height) return;

    uint32_t offset = y * (fb_pitch / 4) + x;
    fb_buffer[offset] = color;
}

// Fill a rectangle with a solid colour (clipped to screen bounds)
void fb_fill_rect(uint32_t x, uint32_t y,
                  uint32_t width, uint32_t height, uint32_t color) {
    if (x >= fb_width || y >= fb_height) return;
    if (x + width  > fb_width)  width  = fb_width  - x;
    if (y + height > fb_height) height = fb_height - y;

    uint32_t words_per_row = fb_pitch / 4;
    for (uint32_t row = 0; row < height; row++) {
        uint32_t* line = fb_buffer + (y + row) * words_per_row + x;
        for (uint32_t col = 0; col < width; col++) {
            line[col] = color;
        }
    }
}

// Clear the entire screen to a single colour
void fb_clear(uint32_t color) {
    if (!fb_buffer || fb_width == 0 || fb_height == 0) return;
    fb_fill_rect(0, 0, fb_width, fb_height, color);
}

// Accessors
uint32_t  fb_get_width()  { return fb_width;  }
uint32_t  fb_get_height() { return fb_height; }
uint32_t  fb_get_pitch()  { return fb_pitch;  }
uint32_t* fb_get_buffer() { return fb_buffer; }