#ifndef MULTIBOOT2_H
#define MULTIBOOT2_H

#include <stdint.h>

/*
 * Multiboot2 magic values and tag types.
 * See: https://www.gnu.org/software/grub/manual/multiboot2/multiboot.html
 */

#define MB2_BOOTLOADER_MAGIC 0x36D76289u   // Passed in EAX
#define MB2_HEADER_MAGIC     0xE85250D6u   // Kernel header magic

// Tag type identifiers
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

// Memory region types (from MMAP tag)
#define MB2_MMAP_TYPE_AVAILABLE 1
#define MB2_MMAP_TYPE_RESERVED  2
#define MB2_MMAP_TYPE_ACPI      3
#define MB2_MMAP_TYPE_NVS       4
#define MB2_MMAP_TYPE_BADRAM    5

#define MB2_MMAP_MAX_ENTRIES 128

// Generic tag header
struct mb2_tag {
    uint32_t type;
    uint32_t size;
} __attribute__((packed));

// Basic memory info tag (type 4)
struct mb2_tag_basic_meminfo {
    uint32_t type;
    uint32_t size;
    uint32_t mem_lower;   // Conventional memory in KB (0-640)
    uint32_t mem_upper;   // Extended memory in KB (1MB+)
} __attribute__((packed));

// Memory map entry
struct mb2_mmap_entry {
    uint64_t addr;   // Base address
    uint64_t len;    // Length in bytes
    uint32_t type;   // Region type (available, reserved, etc.)
    uint32_t zero;   // Reserved, must be zero
} __attribute__((packed));

// MMAP tag (type 6)
struct mb2_tag_mmap {
    uint32_t type;
    uint32_t size;
    uint32_t entry_size;     // Size of each entry
    uint32_t entry_version;  // Version (must be 0)
    struct mb2_mmap_entry entries[];
} __attribute__((packed));

// Framebuffer tag (type 8)
struct mb2_tag_framebuffer {
    uint32_t type;
    uint32_t size;
    uint64_t framebuffer_addr;
    uint32_t framebuffer_pitch;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint8_t  framebuffer_bpp;
    uint8_t  framebuffer_type;   // 0=indexed, 1=RGB, 2=EGA
    uint16_t reserved;
    // Color info follows (variable, depends on framebuffer_type)
} __attribute__((packed));

// String tag (type 1,2, etc.)
struct mb2_tag_string {
    uint32_t type;
    uint32_t size;
    char     string[];
} __attribute__((packed));

// Flat memory map entry for 32-bit kernel
typedef struct {
    uint64_t base;   // Base address (preserved from MB2, may be >4 GB on PAE systems)
    uint64_t len;    // Length in bytes (preserved from MB2)
    uint32_t type;   // Region type
} __attribute__((packed)) mb2_mmap_flat_t;

// Flat MMAP table passed to physical memory manager
typedef struct {
    mb2_mmap_flat_t entries[MB2_MMAP_MAX_ENTRIES];
    uint32_t        count;
} mb2_mmap_table_t;

// Multiboot2 module tag (type 3) — one per `module2` line in grub.cfg.
struct mb2_tag_module {
    uint32_t type;        // == MB2_TAG_MODULE
    uint32_t size;
    uint32_t mod_start;   // physical address of module start
    uint32_t mod_end;     // physical address of module end (exclusive)
    char     string[];    // NUL-terminated cmdline / identifier
} __attribute__((packed));

// Captured module info passed back from multiboot2_parse(); only the first
// module whose cmdline starts with "cctkfs" is recorded for now.
#define MB2_MODULE_NAME_MAX 32
typedef struct {
    uint32_t mod_start;     // 0 when no matching module was found
    uint32_t mod_size;
    char     name[MB2_MODULE_NAME_MAX];
} mb2_module_info_t;

#endif  