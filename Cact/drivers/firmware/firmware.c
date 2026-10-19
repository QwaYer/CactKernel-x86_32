#include "firmware.h"
#include "initfs_modblob.h"
#include "vfs.h"
#include "memory.h"
#include "klib.h"
#include "kernel.h"
#include "cact_zstd.h"

#define FW_DIR "/usr/lib/firmware/"

/* ------------------------------------------------------------------ */
/* cctkfs: blobs baked into the initfs image the bootloader hands us.  */
/* ------------------------------------------------------------------ */

/* Look a path up in the cctkfs archive and return a private copy.  The archive
 * itself is a shared staging buffer, so callers must not hold into it across
 * other lookups. */
static uint8_t *fw_load_initfs(const char *path, uint32_t *out_len) {
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

/* Same, for a zstd frame: the frame header carries the uncompressed size, so
 * the destination is allocated up front and the blob is unpacked into it. */
static uint8_t *fw_load_zst_initfs(const char *path, uint32_t *out_len) {
    const uint8_t *blob = NULL;
    uint32_t comp_size = 0;
    size_t raw_size;
    uint8_t *copy;

    if (initfs_modblob_get(path, &blob, &comp_size) != 0 || !blob || !comp_size)
        return NULL;

    raw_size = cact_zstd_frame_size(blob, comp_size);
    if (!raw_size)
        return NULL;

    copy = (uint8_t *)kmalloc((uint32_t)raw_size);
    if (!copy)
        return NULL;

    /* still inside the shared staging buffer — decompress before returning */
    if (cact_zstd_decompress(blob, comp_size, copy, raw_size) != raw_size) {
        kfree(copy);
        return NULL;
    }

    *out_len = (uint32_t)raw_size;
    return copy;
}

/* ------------------------------------------------------------------ */
/* VFS: a blob on disk — extracted by firmextr, or installed by cactpkg. */
/* ------------------------------------------------------------------ */

static uint8_t *fw_load_vfs(const char *path, uint32_t *out_len) {
    if (!vfs_root)
        return NULL;

    vfs_node_t *node = vfs_resolve_path(path);
    if (!node)
        return NULL;

    if (node->type != VFS_FILE || !node->size) {
        vfs_node_unref(node);
        return NULL;
    }

    uint32_t size = node->size;
    uint8_t *copy = (uint8_t *)kmalloc(size);
    if (!copy) {
        vfs_node_unref(node);
        return NULL;
    }

    int br = read_vfs(node, 0, size, (char *)copy);
    vfs_node_unref(node);
    if (br <= 0) {
        kfree(copy);
        return NULL;
    }

    *out_len = size;
    return copy;
}

static uint8_t *fw_load_zst_vfs(const char *path, uint32_t *out_len) {
    uint32_t comp_size = 0;
    uint8_t *comp = fw_load_vfs(path, &comp_size);
    if (!comp)
        return NULL;

    size_t raw_size = cact_zstd_frame_size(comp, comp_size);
    if (!raw_size) {
        kfree(comp);
        return NULL;
    }

    uint8_t *copy = (uint8_t *)kmalloc((uint32_t)raw_size);
    if (!copy) {
        kfree(comp);
        return NULL;
    }

    if (cact_zstd_decompress(comp, comp_size, copy, raw_size) != raw_size) {
        kfree(comp);
        kfree(copy);
        return NULL;
    }

    kfree(comp);
    *out_len = (uint32_t)raw_size;
    return copy;
}

/* ------------------------------------------------------------------ */
/* Lookup: initfs first (RAM, always present), then the VFS.           */
/* ------------------------------------------------------------------ */

static uint8_t *fw_load_any(const char *path, uint32_t *out_len) {
    uint8_t *copy = fw_load_initfs(path, out_len);
    return copy ? copy : fw_load_vfs(path, out_len);
}

static uint8_t *fw_load_zst_any(const char *path, uint32_t *out_len) {
    uint8_t *copy = fw_load_zst_initfs(path, out_len);
    return copy ? copy : fw_load_zst_vfs(path, out_len);
}

int request_firmware(const char *name, const uint8_t **data, uint32_t *len) {
    if (!name || !*name || !data || !len)
        return -1;

    char path[160];
    uint32_t size = 0;
    uint8_t *copy;

    /* The staged form is compressed: <name>.zst.  An uncompressed <name> is
     * also accepted (and the /usr/lib shape too), so hand-placed blobs keep
     * working.  Each shape is tried in initfs, then on the VFS. */
    snprintf(path, sizeof(path), "%s%s.zst", FW_DIR, name);
    copy = fw_load_zst_any(path, &size);
    if (!copy) {
        snprintf(path, sizeof(path), "%s%s", FW_DIR, name);
        copy = fw_load_any(path, &size);
    }
    if (!copy) {
        snprintf(path, sizeof(path), "/usr/lib/%s.zst", name);
        copy = fw_load_zst_any(path, &size);
    }
    if (!copy) {
        snprintf(path, sizeof(path), "/usr/lib/%s", name);
        copy = fw_load_any(path, &size);
    }
    if (!copy) {
        pr_warn("firmware: %s not found in cctkfs or vfs\n", name);
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
