#include "font.h"
#include "initfs_modblob.h"
#include "kernel.h"

static console_font_t boot_font;
static int            boot_font_ready;

int font_load_boot(const char *path) {
    const uint8_t *blob;
    uint32_t       size;

    if (!path || initfs_modblob_get(path, &blob, &size) != 0) {
        pr_err("  %-11s : boot font not staged in cctkfs (%s)\n",
               "font", path ? path : "(null)");
        return FONT_ERR_NOT_FOUND;
    }

    int rc = font_parse_psf2(blob, size, &boot_font);
    boot_font_ready = (rc == FONT_OK);
    if (rc != FONT_OK) {
        pr_err("  %-11s : PSF2 parse failed (rc=%d, %u bytes)\n",
               "font", rc, (unsigned)size);
        return rc;
    }

    pr_info("  %-11s : boot font %ux%u, %u glyphs (%u B)\n",
            "font", (unsigned)boot_font.width, (unsigned)boot_font.height,
            (unsigned)boot_font.num_glyphs, (unsigned)size);
    return rc;
}

const console_font_t *font_get_active(void) {
    return boot_font_ready ? &boot_font : NULL;
}
