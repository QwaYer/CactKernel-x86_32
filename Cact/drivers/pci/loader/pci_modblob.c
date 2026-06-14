#include "pci_modblob.h"
#include "klib.h"
#include "kernel.h"
#include "cctkfs.h"

/* HMAC tag size — must match CACT_HMAC_TAG_SIZE in pci_loader.c */
#define MODBLOB_HMAC_TAG_SIZE  32

CACT_STATIC_ASSERT(MODBLOB_HMAC_TAG_SIZE == 32);
CACT_STATIC_ASSERT(sizeof(uint32_t)       == 4);

/* ------------------------------------------------------------------ */
/* CRC-32 (IEEE 802.3) — container-level integrity for the cctkfs     */
/* archive.  Each module is still individually HMAC-SHA256-signed;     */
/* this CRC32 only guards against accidental corruption of the index.  */
/* ------------------------------------------------------------------ */
static const uint32_t crc32_tab[256] = {
    0x00000000u, 0x77073096u, 0xEE0E612Cu, 0x990951BAu,
    0x076DC419u, 0x706AF48Fu, 0xE963A535u, 0x9E6495A3u,
    0x0EDB8832u, 0x79DCB8A4u, 0xE0D5E91Eu, 0x97D2D988u,
    0x09B64C2Bu, 0x7EB17CBDu, 0xE7B82D07u, 0x90BF1D91u,
    0x1DB71064u, 0x6AB020F2u, 0xF3B97148u, 0x84BE41DEu,
    0x1ADAD47Du, 0x6DDDE4EBu, 0xF4D4B551u, 0x83D385C7u,
    0x136C9856u, 0x646BA8C0u, 0xFD62F97Au, 0x8A65C9ECu,
    0x14015C4Fu, 0x63066CD9u, 0xFA0F3D63u, 0x8D080DF5u,
    0x3B6E20C8u, 0x4C69105Eu, 0xD56041E4u, 0xA2677172u,
    0x3C03E4D1u, 0x4B04D447u, 0xD20D85FDu, 0xA50AB56Bu,
    0x35B5A8FAu, 0x42B2986Cu, 0xDBBBC9D6u, 0xACBCF940u,
    0x32D86CE3u, 0x45DF5C75u, 0xDCD60DCFu, 0xABD13D59u,
    0x26D930ACu, 0x51DE003Au, 0xC8D75180u, 0xBFD06116u,
    0x21B4F4B5u, 0x56B3C423u, 0xCFBA9599u, 0xB8BDA50Fu,
    0x2802B89Eu, 0x5F058808u, 0xC60CD9B2u, 0xB10BE924u,
    0x2F6F7C87u, 0x58684C11u, 0xC1611DABu, 0xB6662D3Du,
    0x76DC4190u, 0x01DB7106u, 0x98D220BCu, 0xEFD5102Au,
    0x71B18589u, 0x06B6B51Fu, 0x9FBFE4A5u, 0xE8B8D433u,
    0x7807C9A2u, 0x0F00F934u, 0x9609A88Eu, 0xE10E9818u,
    0x7F6A0DBBu, 0x086D3D2Du, 0x91646C97u, 0xE6635C01u,
    0x6B6B51F4u, 0x1C6C6162u, 0x856530D8u, 0xF262004Eu,
    0x6C0695EDu, 0x1B01A57Bu, 0x8208F4C1u, 0xF50FC457u,
    0x65B0D9C6u, 0x12B7E950u, 0x8BBEB8EAu, 0xFCB9887Cu,
    0x62DD1DDFu, 0x15DA2D49u, 0x8CD37CF3u, 0xFBD44C65u,
    0x4DB26158u, 0x3AB551CEu, 0xA3BC0074u, 0xD4BB30E2u,
    0x4ADFA541u, 0x3DD895D7u, 0xA4D1C46Du, 0xD3D6F4FBu,
    0x4369E96Au, 0x346ED9FCu, 0xAD678846u, 0xDA60B8D0u,
    0x44042D73u, 0x33031DE5u, 0xAA0A4C5Fu, 0xDD0D7CC9u,
    0x5005713Cu, 0x270241AAu, 0xBE0B1010u, 0xC90C2086u,
    0x5768B525u, 0x206F85B3u, 0xB966D409u, 0xCE61E49Fu,
    0x5EDEF90Eu, 0x29D9C998u, 0xB0D09822u, 0xC7D7A8B4u,
    0x59B33D17u, 0x2EB40D81u, 0xB7BD5C3Bu, 0xC0BA6CADu,
    0xEDB88320u, 0x9ABFB3B6u, 0x03B6E20Cu, 0x74B1D29Au,
    0xEAD54739u, 0x9DD277AFu, 0x04DB2615u, 0x73DC1683u,
    0xE3630B12u, 0x94643B84u, 0x0D6D6A3Eu, 0x7A6A5AA8u,
    0xE40ECF0Bu, 0x9309FF9Du, 0x0A00AE27u, 0x7D079EB1u,
    0xF00F9344u, 0x8708A3D2u, 0x1E01F268u, 0x6906C2FEu,
    0xF762575Du, 0x806567CBu, 0x196C3671u, 0x6E6B06E7u,
    0xFED41B76u, 0x89D32BE0u, 0x10DA7A5Au, 0x67DD4ACCu,
    0xF9B9DF6Fu, 0x8EBEEFF9u, 0x17B7BE43u, 0x60B08ED5u,
    0xD6D6A3E8u, 0xA1D1937Eu, 0x38D8C2C4u, 0x4FDFF252u,
    0xD1BB67F1u, 0xA6BC5767u, 0x3FB506DDu, 0x48B2364Bu,
    0xD80D2BDAu, 0xAF0A1B4Cu, 0x36034AF6u, 0x41047A60u,
    0xDF60EFC3u, 0xA867DF55u, 0x316E8EEFu, 0x4669BE79u,
    0xCB61B38Cu, 0xBC66831Au, 0x256FD2A0u, 0x5268E236u,
    0xCC0C7795u, 0xBB0B4703u, 0x220216B9u, 0x5505262Fu,
    0xC5BA3BBEu, 0xB2BD0B28u, 0x2BB45A92u, 0x5CB36A04u,
    0xC2D7FFA7u, 0xB5D0CF31u, 0x2CD99E8Bu, 0x5BDEAE1Du,
    0x9B64C2B0u, 0xEC63F226u, 0x756AA39Cu, 0x026D930Au,
    0x9C0906A9u, 0xEB0E363Fu, 0x72076785u, 0x05005713u,
    0x95BF4A82u, 0xE2B87A14u, 0x7BB12BAEu, 0x0CB61B38u,
    0x92D28E9Bu, 0xE5D5BE0Du, 0x7CDCEFB7u, 0x0BDBDF21u,
    0x86D3D2D4u, 0xF1D4E242u, 0x68DDB3F8u, 0x1FDA836Eu,
    0x81BE16CDu, 0xF6B9265Bu, 0x6FB077E1u, 0x18B74777u,
    0x88085AE6u, 0xFF0F6A70u, 0x66063BCAu, 0x11010B5Cu,
    0x8F659EFFu, 0xF862AE69u, 0x616BFFD3u, 0x166CCF45u,
    0xA00AE278u, 0xD70DD2EEu, 0x4E048354u, 0x3903B3C2u,
    0xA7672661u, 0xD06016F7u, 0x4969474Du, 0x3E6E77DBu,
    0xAED16A4Au, 0xD9D65ADCu, 0x40DF0B66u, 0x37D83BF0u,
    0xA9BCAE53u, 0xDEBB9EC5u, 0x47B2CF7Fu, 0x30B5FFE9u,
    0xBDBDF21Cu, 0xCABAC28Au, 0x53B39330u, 0x24B4A3A6u,
    0xBAD03605u, 0xCDD70693u, 0x54DE5729u, 0x23D967BFu,
    0xB3667A2Eu, 0xC4614AB8u, 0x5D681B02u, 0x2A6F2B94u,
    0xB40BBE37u, 0xC30C8EA1u, 0x5A05DF1Bu, 0x2D02EF8Du,
};

static uint32_t crc32(const uint8_t *buf, uint32_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < len; i++)
        crc = crc32_tab[(crc ^ buf[i]) & 0xFFu] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

/* The on-disk checksum covers the entire image *except* that the
 * 4-byte checksum field itself is treated as 0 during computation. */
static uint32_t compute_image_crc32(const uint8_t *img, uint32_t total) {
    uint32_t c = crc32(img, CCTKFS_CKSUM_OFF);
    for (uint32_t i = 0; i < 4 && (CCTKFS_CKSUM_OFF + i) < total; i++)
        c = crc32_tab[c & 0xFFu] ^ (c >> 8);
    uint32_t after = CCTKFS_CKSUM_OFF + 4;
    if (after < total)
        c = crc32(img + after, total - after);
    return c;
}

/* ------------------------------------------------------------------ */
/* Staging area for the cctkfs image handed to us by GRUB.            */
/* ------------------------------------------------------------------ */
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
    if (h->names_off + h->names_size < h->names_off ||
        h->names_off + h->names_size > size)          return -6;
    if (h->count > (UINT32_MAX / sizeof(cctkfs_entry_t))) return -7;
    uint32_t entries_end = h->entries_off + h->count * sizeof(cctkfs_entry_t);
    if (entries_end < h->entries_off ||
        entries_end > h->names_off)    return -7;

    /* CRC-32 container integrity check.
     * checksum == 0  →  backward-compatible with old images that
     *                    had the field named 'reserved' (zero-initialised). */
    if (h->checksum != 0) {
        uint32_t expected = compute_image_crc32(cctkfs_stage, size);
        if (h->checksum != expected) {
            kprint("[MODBLOB] cctkfs checksum mismatch: got 0x");
            char nb[12];
            hex_to_ascii(h->checksum, nb); kprint(nb);
            kprint(", expected 0x");
            hex_to_ascii(expected, nb); kprint(nb);
            kprint("\n");
            return -8;
        }
    }

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
        uint32_t data_end = e[i].data_off + e[i].data_size;
        if (data_end < e[i].data_off || data_end > cctkfs_size) return -1;
        *out_data = cctkfs_stage + e[i].data_off;
        *out_size = e[i].data_size;
        return 0;
    }

    const char *want_bn = basename_of(path);
    if (!*want_bn) return -1;
    for (uint32_t i = 0; i < h->count; i++) {
        if (!basename_eq(want_bn, n, e[i].name_off, e[i].name_len)) continue;
        uint32_t data_end = e[i].data_off + e[i].data_size;
        if (data_end < e[i].data_off || data_end > cctkfs_size) return -1;
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
    uint32_t data_end = e->data_off + e->data_size;
    if (data_end < e->data_off || data_end > cctkfs_size) return -1;
    /* Names in the blob are NUL-terminated (the packer guarantees it). */
    uint32_t name_end = e->name_off + e->name_len;
    if (name_end < e->name_off || name_end > h->names_size) return -1;
    *out_path = names_ptr() + e->name_off;
    *out_data = cctkfs_stage + e->data_off;
    *out_size = e->data_size;
    return 0;
}
