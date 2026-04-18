#include "kernel.h"
#include "multiboot2.h"

#define MB2_FLAG_MEMINFO     (1u << 0)
#define MB2_FLAG_MMAP        (1u << 6)
#define MB2_FLAG_FRAMEBUFFER (1u << 12)

void multiboot2_parse(uint32_t mb2_info_addr, multiboot_info_t* out)
{
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

    if (mb2_info_addr == 0) return;

    /* Multiboot2 info block header: total_size (u32), reserved (u32). */
    uint32_t total_size = *(volatile uint32_t*)(uintptr_t)mb2_info_addr;
    if (total_size < 8) return;

    uint32_t cursor = mb2_info_addr + 8;
    uint32_t end    = mb2_info_addr + total_size;

    while (cursor + sizeof(struct mb2_tag) <= end) {
        struct mb2_tag* tag = (struct mb2_tag*)(uintptr_t)cursor;
        if (tag->type == MB2_TAG_END) break;
        if (tag->size < sizeof(struct mb2_tag)) break;

        switch (tag->type) {
        case MB2_TAG_BASIC_MEMINFO: {
            struct mb2_tag_basic_meminfo* t = (struct mb2_tag_basic_meminfo*)tag;
            out->mem_lower = t->mem_lower;
            out->mem_upper = t->mem_upper;
            out->flags |= MB2_FLAG_MEMINFO;
            break;
        }
        case MB2_TAG_MMAP: {
            struct mb2_tag_mmap* t = (struct mb2_tag_mmap*)tag;
            if (t->entry_size == 0 || t->size < 16) break;
            uint32_t entries_bytes = t->size - 16;
            uint32_t count = entries_bytes / t->entry_size;
            uint64_t total = 0;
            for (uint32_t i = 0; i < count; i++) {
                struct mb2_mmap_entry* e =
                    (struct mb2_mmap_entry*)((uint8_t*)t->entries + i * t->entry_size);
                if (e->type == MB2_MMAP_TYPE_AVAILABLE)
                    total += e->len;
            }
            out->mem_total_bytes = total;
            out->flags |= MB2_FLAG_MMAP;
            break;
        }
        case MB2_TAG_FRAMEBUFFER: {
            struct mb2_tag_framebuffer* t = (struct mb2_tag_framebuffer*)tag;
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

        /* Tags are 8-byte aligned. */
        uint32_t adv = (tag->size + 7u) & ~7u;
        if (adv == 0) break;
        cursor += adv;
    }
}
