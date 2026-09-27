#include "firmware.h"
#include "initfs_modblob.h"
#include "memory.h"
#include "klib.h"
#include "kernel.h"

#define FW_DIR "/lib/firmware/"

/* Look a path up in the cctkfs archive and return a private copy.  The archive
 * itself is a shared staging buffer, so callers must not hold into it across
 * other lookups. */
static uint8_t *fw_load(const char *path, uint32_t *out_len) {
    const uint8_t *blob = NULL;
    uint32_t size = 0;

    if (initfs_modblob_get(path, &blob, &size) != 0 || !blob || !size)
        return NULL;

    uint8_t *copy = (uint8_t *)kmalloc(size);
    if (!copy)
        return NULL;

    memcpy(copy, blob, size);
    *out_len = size;
    return copy;
}

int request_firmware(const char *name, const uint8_t **data, uint32_t *len) {
    if (!name || !*name || !data || !len)
        return -1;

    char path[160];
    uint32_t size = 0;
    uint8_t *copy;

    snprintf(path, sizeof(path), "%s%s", FW_DIR, name);
    copy = fw_load(path, &size);
    if (!copy) {
        /* The packer also accepts /lib/<name>; allow that shape too. */
        snprintf(path, sizeof(path), "/lib/%s", name);
        copy = fw_load(path, &size);
    }
    if (!copy) {
        pr_warn("[firmware] %s not found in cctkfs\n", name);
        return -1;
    }

    *data = copy;
    *len  = size;
    return 0;
}

void release_firmware(const uint8_t *data) {
    if (data)
        kfree((void *)data);
}
