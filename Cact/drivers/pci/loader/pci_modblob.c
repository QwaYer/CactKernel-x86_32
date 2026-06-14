#include "pci_modblob.h"
#include "klib.h"
#include "kernel.h"
#include "cctkfs.h"

/* Staging area for the cctkfs image handed to us by GRUB.  Lives in .bss
 * (zero-initialised, takes no ISO space) so the heap/PMM cannot stomp on
 * it after pmm_init_from_mmap() / init_heap() run. */
static uint8_t  cctkfs_stage[PCI_MODBLOB_MAX_IMAGE];
static uint32_t cctkfs_size;
static int      cctkfs_ready;

static const cctkfs_hdr_t   *hdr_ptr(void) {
    return (const cctkfs_hdr_t *)cctkfs_stage;
}
static const cctkfs_entry_t *entries_ptr(void) {
    return (const cctkfs_entry_t *)(cctkfs_stage + hdr_ptr()->entries_off);
}
static const char *names_ptr(void) {
    return (const char *)(cctkfs_stage + hdr_ptr()->names_off);
}

static int validate_header(uint32_t size) {
    if (size < sizeof(cctkfs_hdr_t)) return -1;
    const cctkfs_hdr_t *h = hdr_ptr();
    if (h->magic   != CCTKFS_MAGIC)   return -2;
    if (h->version != CCTKFS_VERSION) return -3;
    if (h->total_size != size)        return -4;
    if (h->entries_off < sizeof(cctkfs_hdr_t)) return -5;
    if (h->names_off + h->names_size > size)   return -6;
    if (h->entries_off + h->count * sizeof(cctkfs_entry_t) > h->names_off)
        return -7;
    return 0;
}

int pci_modblob_load(uint32_t phys_addr, uint32_t size) {
    if (!phys_addr || !size) {
        kprint("[MODBLOB] no cctkfs module supplied by bootloader\n");
        return -1;
    }
    if (size > PCI_MODBLOB_MAX_IMAGE) {
        kprint("[MODBLOB] cctkfs image too large for stage buffer\n");
        return -2;
    }

    /* GRUB places the module in low physical memory.  We are still in
     * flat protected mode without paging, so a plain memcpy works. */
    memcpy(cctkfs_stage, (const void *)(uintptr_t)phys_addr, size);
    cctkfs_size = size;

    int rc = validate_header(size);
    if (rc != 0) {
        kprint("[MODBLOB] cctkfs header invalid (rc=");
        char nb[8]; itoa(rc, nb); kprint(nb);
        kprint(")\n");
        cctkfs_size = 0;
        return rc;
    }

    cctkfs_ready = 1;

    char nb[16];
    itoa((int)hdr_ptr()->count, nb);
    kprint("[MODBLOB] ready: ");
    kprint(nb);
    kprint(" mods, ");
    itoa((int)size, nb); kprint(nb); kprint(" B\n");
    return 0;
}

static int name_eq(const char *want, const char *blob, uint32_t off,
                   uint32_t len) {
    for (uint32_t i = 0; i < len; i++) {
        if (want[i] == '\0')              return 0;
        if (want[i] != blob[off + i])     return 0;
    }
    return want[len] == '\0';
}

/* basename helpers: cctkfs entries are stored as canonical paths like
 * "/lib/foo.cctk", but callers may use any equivalent location, e.g.
 * "/proc/bin/mdls/foo.cctk" (the user-visible mountpoint) or just "foo.cctk".
 * Fall back to a basename match so all of those resolve to the same module. */
static const char *basename_of(const char *path) {
    const char *last = path;
    for (const char *s = path; *s; s++)
        if (*s == '/') last = s + 1;
    return last;
}

static int basename_eq(const char *want_basename,
                       const char *blob, uint32_t off, uint32_t len) {
    uint32_t blob_basename_off = off;
    for (uint32_t i = 0; i < len; i++)
        if (blob[off + i] == '/') blob_basename_off = off + i + 1;
    uint32_t blob_basename_len = (off + len) - blob_basename_off;
    return name_eq(want_basename, blob, blob_basename_off, blob_basename_len);
}

int pci_modblob_get(const char *path, const uint8_t **out_data,
                    uint32_t *out_size) {
    if (!cctkfs_ready || !path || !out_data || !out_size) return -1;
    const cctkfs_hdr_t   *h = hdr_ptr();
    const cctkfs_entry_t *e = entries_ptr();
    const char           *n = names_ptr();

    for (uint32_t i = 0; i < h->count; i++) {
        if (!name_eq(path, n, e[i].name_off, e[i].name_len)) continue;
        if (e[i].data_off + e[i].data_size > cctkfs_size)    return -1;
        *out_data = cctkfs_stage + e[i].data_off;
        *out_size = e[i].data_size;
        return 0;
    }

    const char *want_bn = basename_of(path);
    if (!*want_bn) return -1;
    for (uint32_t i = 0; i < h->count; i++) {
        if (!basename_eq(want_bn, n, e[i].name_off, e[i].name_len)) continue;
        if (e[i].data_off + e[i].data_size > cctkfs_size) return -1;
        *out_data = cctkfs_stage + e[i].data_off;
        *out_size = e[i].data_size;
        return 0;
    }
    return -1;
}

int pci_modblob_count(void) {
    return cctkfs_ready ? (int)hdr_ptr()->count : 0;
}

int pci_modblob_at(int idx, const char **out_path,
                   const uint8_t **out_data, uint32_t *out_size) {
    if (!cctkfs_ready) return -1;
    const cctkfs_hdr_t   *h = hdr_ptr();
    if (idx < 0 || (uint32_t)idx >= h->count) return -1;
    const cctkfs_entry_t *e = &entries_ptr()[idx];
    /* Names in the blob are NUL-terminated (the packer guarantees it). */
    *out_path = names_ptr() + e->name_off;
    *out_data = cctkfs_stage + e->data_off;
    *out_size = e->data_size;
    return 0;
}
