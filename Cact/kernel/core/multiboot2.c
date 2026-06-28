#include "kernel.h"
#include "multiboot2.h"

// Flags for which fields are valid in multiboot_info_t
#define MB2_FLAG_MEMINFO     (1u << 0)
#define MB2_FLAG_MMAP        (1u << 6)
#define MB2_FLAG_FRAMEBUFFER (1u << 12)

#define MB2_TAG_HDR_SIZE  8u
#define MB2_ALIGN         8u

// Module info captured from MB2_TAG_MODULE; written by multiboot2_parse(),
// consumed by init() to stage the cctkfs image into kernel .bss.
mb2_module_info_t mb2_cctkfs_module;

static int starts_with(const char* s, const char* prefix) {
    while (*prefix) {
        if (*s++ != *prefix++) return 0;
    }
    return 1;
}

static void copy_module_name(char* dst, const char* src, uint32_t max) {
    uint32_t i = 0;
    while (i + 1 < max && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

// Parse multiboot2 tag list into simplified kernel structure
void multiboot2_parse(uint32_t mb2_info_addr,
                      multiboot_info_t*  out,
                      mb2_mmap_table_t*  mmap_out)
{
    mb2_cctkfs_module.mod_start = 0;
    mb2_cctkfs_module.mod_size  = 0;
    mb2_cctkfs_module.name[0]   = '\0';

    // Zero output structures
    out->flags             = 0;
    out->mem_lower         = 0;
    out->mem_upper         = 0;
    out->mem_total_bytes   = 0;
    out->framebuffer_addr  = 0;
    out->framebuffer_pitch = 0;
    out->framebuffer_width = 0;
    out->framebuffer_height= 0;
    out->framebuffer_bpp   = 0;
    out->framebuffer_type  = 0;

    if (mmap_out) {
        mmap_out->count = 0;
    }

    // Validate input address
    if (mb2_info_addr == 0) return;
    if (mb2_info_addr & (MB2_ALIGN - 1u)) return;  // Not aligned

    uint32_t total_size = *(volatile uint32_t*)(uintptr_t)mb2_info_addr;
    if (total_size < 16u) return;
    if (mb2_info_addr > (0xFFFFFFFFu - total_size)) return;  // Wraparound

    uint32_t cursor = mb2_info_addr + MB2_ALIGN;  // Skip total_size field
    uint32_t end    = mb2_info_addr + total_size;

    while (1) {
        // Bounds check
        if (cursor > end || (end - cursor) < MB2_TAG_HDR_SIZE) break;

        // Align cursor to 8-byte boundary
        if (cursor & (MB2_ALIGN - 1u)) {
            cursor = (cursor + MB2_ALIGN - 1u) & ~(MB2_ALIGN - 1u);
            continue;
        }

        struct mb2_tag* tag = (struct mb2_tag*)(uintptr_t)cursor;

        // Validate tag header
        if (tag->size < MB2_TAG_HDR_SIZE) break;
        if (cursor > end || (end - cursor) < tag->size) break;

        // End tag -> terminate parsing
        if (tag->type == MB2_TAG_END) break;

        switch (tag->type) {

        case MB2_TAG_BASIC_MEMINFO: {
            if (tag->size < sizeof(struct mb2_tag_basic_meminfo)) break;
            struct mb2_tag_basic_meminfo* t =
                (struct mb2_tag_basic_meminfo*)(uintptr_t)cursor;
            out->mem_lower = t->mem_lower;
            out->mem_upper = t->mem_upper;
            out->flags |= MB2_FLAG_MEMINFO;
            break;
        }

        case MB2_TAG_MMAP: {
            if (tag->size < 16u) break;
            struct mb2_tag_mmap* t = (struct mb2_tag_mmap*)(uintptr_t)cursor;

            // entry_size must be at least sizeof(mb2_mmap_entry)
            if (t->entry_size < sizeof(struct mb2_mmap_entry)) break;

            uint32_t payload_bytes = tag->size - 16u;
            uint32_t count = payload_bytes / t->entry_size;
            uint64_t total_avail = 0;

            for (uint32_t i = 0; i < count; i++) {
                uint64_t entry_off64 = 16ull + (uint64_t)i * t->entry_size;
                if (entry_off64 > 0xFFFFFFFFu) break;
                uint32_t entry_off = (uint32_t)entry_off64;
                if (entry_off + sizeof(struct mb2_mmap_entry) > tag->size) break;

                struct mb2_mmap_entry* e =
                    (struct mb2_mmap_entry*)((uint8_t*)(uintptr_t)cursor + entry_off);

                // Sum available RAM for total memory count
                if (e->type == MB2_MMAP_TYPE_AVAILABLE) {
                    total_avail += e->len;
                }

                // Build flat mmap table for PMM (32-bit kernel only)
                if (mmap_out && mmap_out->count < MB2_MMAP_MAX_ENTRIES) {
                    // Skip regions entirely above 4GB (unusable in 32-bit)
                    if (e->addr >= 0x100000000ULL) continue;

                    uint64_t end64 = e->addr + e->len;
                    // Clip to 4GB limit
                    if (end64 > 0x100000000ULL) end64 = 0x100000000ULL;
                    uint64_t len64 = end64 - e->addr;
                    if (len64 == 0) continue;

                    mb2_mmap_flat_t* fe =
                        &mmap_out->entries[mmap_out->count++];
                    fe->base = e->addr;
                    fe->len  = len64;
                    fe->type = e->type;
                }
            }

            out->mem_total_bytes = total_avail;
            out->flags |= MB2_FLAG_MMAP;
            break;
        }

        case MB2_TAG_MODULE: {
            if (tag->size <= sizeof(struct mb2_tag_module)) break;
            struct mb2_tag_module* t =
                (struct mb2_tag_module*)(uintptr_t)cursor;
            if (t->mod_end <= t->mod_start) break;

            // Only record the first cctkfs module; any later modules ignored.
            if (mb2_cctkfs_module.mod_start == 0 &&
                starts_with(t->string, "cctkfs"))
            {
                mb2_cctkfs_module.mod_start = t->mod_start;
                mb2_cctkfs_module.mod_size  = t->mod_end - t->mod_start;
                copy_module_name(mb2_cctkfs_module.name, t->string,
                                 MB2_MODULE_NAME_MAX);
            }
            break;
        }

        case MB2_TAG_FRAMEBUFFER: {
            if (tag->size < sizeof(struct mb2_tag_framebuffer)) break;
            struct mb2_tag_framebuffer* t =
                (struct mb2_tag_framebuffer*)(uintptr_t)cursor;
            out->framebuffer_addr   = t->framebuffer_addr;
            out->framebuffer_pitch  = t->framebuffer_pitch;
            out->framebuffer_width  = t->framebuffer_width;
            out->framebuffer_height = t->framebuffer_height;
            out->framebuffer_bpp    = t->framebuffer_bpp;
            out->framebuffer_type   = t->framebuffer_type;
            out->flags |= MB2_FLAG_FRAMEBUFFER;
            break;
        }

        default:
            break;
        }

        // Align tag size to 8 bytes, advance cursor
        uint32_t adv = (tag->size + (MB2_ALIGN - 1u)) & ~(MB2_ALIGN - 1u);
        if (adv < MB2_TAG_HDR_SIZE) break;  // Would loop forever
        if (cursor > end - adv) break;       // Would overflow
        cursor += adv;
    }
}