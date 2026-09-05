#include "font.h"
#include "initfs_modblob.h"

static console_font_t boot_font;
static int            boot_font_ready;

int font_load_boot(const char *path) {
    const uint8_t *blob;
    uint32_t       size;

    if (!path || initfs_modblob_get(path, &blob, &size) != 0)
        return FONT_ERR_NOT_FOUND;

    int rc = font_parse_psf2(blob, size, &boot_font);
    boot_font_ready = (rc == FONT_OK);
    return rc;
}

const console_font_t *font_get_active(void) {
    return boot_font_ready ? &boot_font : NULL;
}
