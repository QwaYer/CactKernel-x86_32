#ifndef MULTIBOOT2_H
#define MULTIBOOT2_H

#include <stdint.h>

/*
 * Multiboot2 magic values and tag types.
 * See: https://www.gnu.org/software/grub/manual/multiboot2/multiboot.html
 */

#define MB2_BOOTLOADER_MAGIC 0x36D76289u  
#define MB2_HEADER_MAGIC     0xE85250D6u  

#define MB2_TAG_END           0
#define MB2_TAG_CMDLINE       1
#define MB2_TAG_BOOT_LOADER   2
#define MB2_TAG_MODULE        3
#define MB2_TAG_BASIC_MEMINFO 4
#define MB2_TAG_BOOTDEV       5
#define MB2_TAG_MMAP          6
#define MB2_TAG_VBE           7
#define MB2_TAG_FRAMEBUFFER   8
#define MB2_TAG_ELF_SECTIONS  9
#define MB2_TAG_APM           10
#define MB2_TAG_ACPI_OLD      14
#define MB2_TAG_ACPI_NEW      15
#define MB2_TAG_NETWORK       16

#define MB2_MMAP_TYPE_AVAILABLE 1
#define MB2_MMAP_TYPE_RESERVED  2
#define MB2_MMAP_TYPE_ACPI      3
#define MB2_MMAP_TYPE_NVS       4
#define MB2_MMAP_TYPE_BADRAM    5

#define MB2_MMAP_MAX_ENTRIES 128

struct mb2_tag {
    uint32_t type;
    uint32_t size;
} __attribute__((packed));

struct mb2_tag_basic_meminfo {
    uint32_t type;
    uint32_t size;
    uint32_t mem_lower;   /* KB */
    uint32_t mem_upper;   /* KB */
} __attribute__((packed));

struct mb2_mmap_entry {
    uint64_t addr;
    uint64_t len;
    uint32_t type;
    uint32_t zero;
} __attribute__((packed));

struct mb2_tag_mmap {
    uint32_t type;
    uint32_t size;
    uint32_t entry_size;
    uint32_t entry_version;
    struct mb2_mmap_entry entries[];
} __attribute__((packed));

struct mb2_tag_framebuffer {
    uint32_t type;
    uint32_t size;
    uint64_t framebuffer_addr;
    uint32_t framebuffer_pitch;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint8_t  framebuffer_bpp;
    uint8_t  framebuffer_type;   /* 0=indexed, 1=RGB, 2=EGA */
    uint16_t reserved;
    /* color info follows, depends on framebuffer_type */
} __attribute__((packed));

struct mb2_tag_string {
    uint32_t type;
    uint32_t size;
    char     string[];
} __attribute__((packed));

typedef struct {
    uint32_t base;   
    uint32_t len;    
    uint32_t type;   
} mb2_mmap_flat_t;

typedef struct {
    mb2_mmap_flat_t entries[MB2_MMAP_MAX_ENTRIES];
    uint32_t        count;
} mb2_mmap_table_t;

#endif 
