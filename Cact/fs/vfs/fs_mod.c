/*
 * Loadable filesystem-module loader.
 *
 * Mirrors pci_load_module() but for non-PCI filesystems: it loads an ET_REL
 * module (typically `ext4.cctk`) from the staged cctkfs image, verifies its
 * HMAC-SHA256 tag, relocates it, and resolves the generic `fs_mount` entry
 * symbol. mntfs uses fs_mod_mount() to mount the root filesystem at boot.
 *
 * Undefined kernel symbols referenced by the module are resolved through
 * ksym_resolve(), exactly like PCI modules do.
 */

#include "fs_mod.h"
#include "pci_modblob.h"
#include "ksym.h"
#include "klib.h"
#include "memory.h"
#include "kernel.h"

// HMAC-SHA256 module signing — implemented in cact_crypto (Rust, no_std)
extern int cact_hmac_verify(const uint8_t *data, uint32_t data_len,
                            const uint8_t *tag, uint32_t tag_len);

#define CACT_HMAC_TAG_SIZE 32

// ELF 32-bit types (i386)
typedef uint32_t Elf32_Addr;
typedef uint32_t Elf32_Off;
typedef uint16_t Elf32_Half;
typedef uint32_t Elf32_Word;
typedef  int32_t Elf32_Sword;

// ELF constants (relocatable object only)
#define ELF_MAGIC      0x464C457F
#define ET_REL         1
#define EM_386         3
#define SHT_PROGBITS   1
#define SHT_SYMTAB     2
#define SHT_STRTAB     3
#define SHT_REL        9
#define SHF_ALLOC      0x2
#define STB_GLOBAL     1
#define STB_WEAK       2
#define STT_OBJECT     1
#define STT_FUNC       2
#define SHN_UNDEF      0
#define R_386_32       1
#define R_386_PC32     2

#define ELF32_ST_BIND(i)  ((i) >> 4)
#define ELF32_ST_TYPE(i)  ((i) & 0xF)
#define ELF32_R_SYM(i)    ((i) >> 8)
#define ELF32_R_TYPE(i)   ((uint8_t)(i))

typedef struct {
    Elf32_Word e_magic;
    uint8_t    e_class, e_data, e_version2, e_osabi;
    uint8_t    e_pad[8];
    Elf32_Half e_type, e_machine;
    Elf32_Word e_version;
    Elf32_Addr e_entry;
    Elf32_Off  e_phoff, e_shoff;
    Elf32_Word e_flags;
    Elf32_Half e_ehsize, e_phentsize, e_phnum;
    Elf32_Half e_shentsize, e_shnum, e_shstrndx;
} __attribute__((packed)) Elf32_Ehdr;

typedef struct {
    Elf32_Word sh_name, sh_type, sh_flags;
    Elf32_Addr sh_addr;
    Elf32_Off  sh_offset;
    Elf32_Word sh_size, sh_link, sh_info, sh_addralign, sh_entsize;
} __attribute__((packed)) Elf32_Shdr;

typedef struct {
    Elf32_Word st_name;
    Elf32_Addr st_value;
    Elf32_Word st_size;
    uint8_t    st_info, st_other;
    Elf32_Half st_shndx;
} __attribute__((packed)) Elf32_Sym;

typedef struct {
    Elf32_Addr r_offset;
    Elf32_Word r_info;
} __attribute__((packed)) Elf32_Rel;

// Loaded module bookkeeping
static uint8_t      *fs_image;
static uint32_t       fs_image_size;
static fs_mount_fn_t  fs_mount_fn;
static char           fs_instance_name[64];

static Elf32_Shdr *get_shdr(Elf32_Ehdr *eh, uint16_t idx) {
    if (idx >= eh->e_shnum)
        return NULL;
    return (Elf32_Shdr *)((uint8_t *)eh + eh->e_shoff + idx * eh->e_shentsize);
}

static const char *get_str(Elf32_Ehdr *eh, uint16_t strtab_idx, uint32_t off) {
    Elf32_Shdr *sh = get_shdr(eh, strtab_idx);
    if (!sh)
        return NULL;
    return (const char *)((uint8_t *)eh + sh->sh_offset + off);
}

static void module_proc_name(const char *path, char *out, int out_sz) {
    const char *base = path;
    for (const char *p = path; p && *p; p++)
        if (*p == '/') base = p + 1;
    int i = 0;
    while (base[i] && i < out_sz - 1) {
        out[i] = (base[i] == ' ') ? '_' : base[i];
        i++;
    }
    out[i] = '\0';
}

static int hmac_verify_module(uint8_t *elf_data, uint32_t *file_size) {
    if (*file_size <= CACT_HMAC_TAG_SIZE) {
        kprint("[FSMOD] HMAC: unsigned module (no signature) — rejected\n");
        return -1;
    }
    uint32_t  data_len = *file_size - CACT_HMAC_TAG_SIZE;
    uint8_t  *tag      = elf_data + data_len;
    int       rc       = cact_hmac_verify(elf_data, data_len, tag, CACT_HMAC_TAG_SIZE);
    if (rc != 0) {
        kprint("[FSMOD] HMAC: signature mismatch — rejected\n");
        return -1;
    }
    *file_size = data_len;
    for (uint32_t i = 0; i < CACT_HMAC_TAG_SIZE; i++)
        elf_data[data_len + i] = 0;
    return 0;
}

int fs_mod_load(const char *path) {
    if (!path) return -1;
    if (fs_image) {
        kprint("[FSMOD] a filesystem module is already loaded\n");
        return -1;
    }

    const uint8_t *blob_data = NULL;
    uint32_t       blob_size = 0;
    if (pci_modblob_get(path, &blob_data, &blob_size) != 0 || !blob_size) {
        kprint("[FSMOD] module not found: "); kprint((char *)path); kprint("\n");
        return -1;
    }

    uint8_t *elf_data = (uint8_t *)kmalloc(blob_size);
    if (!elf_data) return -1;
    memcpy(elf_data, blob_data, blob_size);
    uint32_t file_size = blob_size;

    if (hmac_verify_module(elf_data, &file_size) != 0) {
        kfree_heap(elf_data);
        return -8;
    }

    Elf32_Ehdr *eh = (Elf32_Ehdr *)elf_data;
    if (eh->e_magic != ELF_MAGIC || eh->e_type != ET_REL || eh->e_machine != EM_386) {
        kprint("[FSMOD] not a valid ELF32 relocatable\n");
        kfree_heap(elf_data);
        return -2;
    }

    uint32_t sh_tab_end;
    if (__builtin_umul_overflow(eh->e_shnum, eh->e_shentsize, &sh_tab_end) ||
        __builtin_uadd_overflow(eh->e_shoff, sh_tab_end, &sh_tab_end) ||
        eh->e_shentsize < sizeof(Elf32_Shdr) || sh_tab_end > file_size) {
        kprint("[FSMOD] corrupted section header table\n");
        kfree_heap(elf_data);
        return -3;
    }

    // First pass: calculate total image size and assign section addresses
    uint32_t total = 0;
    for (uint16_t i = 0; i < eh->e_shnum; i++) {
        Elf32_Shdr *sh = get_shdr(eh, i);
        if (!sh || !(sh->sh_flags & SHF_ALLOC)) continue;
        uint32_t align = sh->sh_addralign ? sh->sh_addralign : 1;
        if (align & (align - 1)) align = 1;
        total = (total + align - 1) & ~(align - 1);
        sh->sh_addr = total;
        total += sh->sh_size;
    }

    uint8_t *image = (uint8_t *)kmalloc(total);
    if (!image) { kfree_heap(elf_data); return -3; }
    memset(image, 0, total);

    // Copy PROGBITS sections into image
    for (uint16_t i = 0; i < eh->e_shnum; i++) {
        Elf32_Shdr *sh = get_shdr(eh, i);
        if (!sh || !(sh->sh_flags & SHF_ALLOC) || sh->sh_type != SHT_PROGBITS) continue;
        if (sh->sh_offset + sh->sh_size > file_size) {
            kprint("[FSMOD] section offset exceeds file\n");
            kfree_heap(image); kfree_heap(elf_data);
            return -8;
        }
        memcpy(image + sh->sh_addr, elf_data + sh->sh_offset, sh->sh_size);
    }

    // Locate symbol table and its string table
    Elf32_Shdr *symtab_sh = NULL;
    uint16_t    strtab_idx = 0;
    for (uint16_t i = 0; i < eh->e_shnum; i++) {
        Elf32_Shdr *sh = get_shdr(eh, i);
        if (sh && sh->sh_type == SHT_SYMTAB) {
            symtab_sh  = sh;
            strtab_idx = (uint16_t)sh->sh_link;
            break;
        }
    }
    if (!symtab_sh) {
        kprint("[FSMOD] no .symtab found\n");
        kfree_heap(image); kfree_heap(elf_data);
        return -4;
    }
    if (symtab_sh->sh_offset + symtab_sh->sh_size > file_size) {
        kfree_heap(image); kfree_heap(elf_data);
        return -8;
    }
    Elf32_Shdr *strtab_sh = get_shdr(eh, strtab_idx);
    if (!strtab_sh || strtab_sh->sh_offset + strtab_sh->sh_size > file_size) {
        kfree_heap(image); kfree_heap(elf_data);
        return -8;
    }
    Elf32_Sym *syms    = (Elf32_Sym *)(elf_data + symtab_sh->sh_offset);
    uint32_t   sym_cnt = symtab_sh->sh_size / sizeof(Elf32_Sym);

    // Apply relocations (R_386_32 and R_386_PC32 only, intra-module + ksym)
    for (uint16_t i = 0; i < eh->e_shnum; i++) {
        Elf32_Shdr *sh = get_shdr(eh, i);
        if (!sh || sh->sh_type != SHT_REL) continue;
        Elf32_Shdr *target_sh = get_shdr(eh, sh->sh_info);
        if (!target_sh || !(target_sh->sh_flags & SHF_ALLOC)) continue;
        Elf32_Rel *rels    = (Elf32_Rel *)(elf_data + sh->sh_offset);
        uint32_t   rel_cnt = sh->sh_size / sizeof(Elf32_Rel);
        for (uint32_t r = 0; r < rel_cnt; r++) {
            uint32_t   sym_idx = ELF32_R_SYM(rels[r].r_info);
            if (sym_idx >= sym_cnt) {
                kprint("[FSMOD] relocation symbol index out of bounds\n");
                kfree_heap(image); kfree_heap(elf_data);
                return -7;
            }
            uint8_t    type    = ELF32_R_TYPE(rels[r].r_info);
            Elf32_Sym *sym     = &syms[sym_idx];
            uint32_t   S;
            if (sym->st_shndx == SHN_UNDEF) {
                const char *sym_name = get_str(eh, strtab_idx, sym->st_name);
                if (!sym_name) { kfree_heap(image); kfree_heap(elf_data); return -7; }
                S = ksym_resolve(sym_name);
                if (S == 0 && ELF32_ST_BIND(sym->st_info) != STB_WEAK) {
                    kprint("[FSMOD] unresolved symbol: "); kprint((char *)sym_name); kprint("\n");
                    kfree_heap(image); kfree_heap(elf_data);
                    return -7;
                }
            } else {
                Elf32_Shdr *sym_sh = get_shdr(eh, sym->st_shndx);
                if (!sym_sh || !(sym_sh->sh_flags & SHF_ALLOC)) {
                    kprint("[FSMOD] bad symbol section\n");
                    kfree_heap(image); kfree_heap(elf_data);
                    return -7;
                }
                S = (uint32_t)(image + sym_sh->sh_addr + sym->st_value);
            }
            if (rels[r].r_offset + sizeof(uint32_t) > target_sh->sh_size) {
                kprint("[FSMOD] relocation offset out of bounds\n");
                kfree_heap(image); kfree_heap(elf_data);
                return -7;
            }
            uint32_t *patch = (uint32_t *)(image + target_sh->sh_addr + rels[r].r_offset);
            if      (type == R_386_32)   *patch += S;
            else if (type == R_386_PC32) *patch += S - (uint32_t)patch;
        }
    }

    // Find exported symbol "fs_mount" (STB_GLOBAL, STT_FUNC)
    fs_mount_fn = NULL;
    for (uint32_t s = 0; s < sym_cnt; s++) {
        if (ELF32_ST_BIND(syms[s].st_info) != STB_GLOBAL) continue;
        if (ELF32_ST_TYPE(syms[s].st_info) != STT_FUNC)   continue;
        const char *sym_name = get_str(eh, strtab_idx, syms[s].st_name);
        if (!sym_name || strcmp((char *)sym_name, "fs_mount") != 0) continue;
        if (syms[s].st_shndx == SHN_UNDEF) continue;
        Elf32_Shdr *sym_sh = get_shdr(eh, syms[s].st_shndx);
        if (!sym_sh) continue;
        fs_mount_fn = (fs_mount_fn_t)(image + sym_sh->sh_addr + syms[s].st_value);
        break;
    }
    if (!fs_mount_fn) {
        kprint("[FSMOD] symbol 'fs_mount' not found\n");
        kfree_heap(image); kfree_heap(elf_data);
        return -5;
    }

    fs_image      = image;
    fs_image_size = total;
    module_proc_name(path, fs_instance_name, sizeof(fs_instance_name));

    kfree_heap(elf_data);
    kprint("[FSMOD] module ready: "); kprint(fs_instance_name); kprint("\n");
    return 0;
}

void fs_mod_unload(void) {
    if (!fs_image) return;
    kfree_heap(fs_image);
    fs_image      = NULL;
    fs_image_size = 0;
    fs_mount_fn   = NULL;
    fs_instance_name[0] = '\0';
    kprint("[FSMOD] module unloaded\n");
}

vfs_node_t *fs_mod_mount(uint32_t dev) {
    if (!fs_mount_fn) return NULL;
    return fs_mount_fn(dev);
}

int fs_mod_loaded(void) {
    return fs_mount_fn != NULL;
}

// Non-destructive probe: scan the ELF symbol table of the module at 'path'
// for an exported global FUNC named 'fs_mount'. Used by GDD to discover
// filesystem modules generically rather than hardcoding a specific FS.
int fs_mod_detect(const char *path) {
    if (!path) return -1;

    const uint8_t *blob = NULL;
    uint32_t       size = 0;
    if (pci_modblob_get(path, &blob, &size) != 0 || !blob || !size)
        return -1;

    Elf32_Ehdr *eh = (Elf32_Ehdr *)blob;
    if (eh->e_magic != ELF_MAGIC || eh->e_type != ET_REL || eh->e_machine != EM_386)
        return 0;

    uint32_t sh_tab_end;
    if (__builtin_umul_overflow(eh->e_shnum, eh->e_shentsize, &sh_tab_end) ||
        __builtin_uadd_overflow(eh->e_shoff, sh_tab_end, &sh_tab_end) ||
        eh->e_shentsize < sizeof(Elf32_Shdr) || sh_tab_end > size)
        return 0;

    Elf32_Shdr *symtab_sh = NULL;
    uint16_t    strtab_idx = 0;
    for (uint16_t i = 0; i < eh->e_shnum; i++) {
        Elf32_Shdr *sh = get_shdr(eh, i);
        if (sh && sh->sh_type == SHT_SYMTAB) {
            symtab_sh  = sh;
            strtab_idx = (uint16_t)sh->sh_link;
            break;
        }
    }
    if (!symtab_sh)
        return 0;
    if (symtab_sh->sh_offset + symtab_sh->sh_size > size)
        return 0;
    Elf32_Shdr *strtab_sh = get_shdr(eh, strtab_idx);
    if (!strtab_sh || strtab_sh->sh_offset + strtab_sh->sh_size > size)
        return 0;

    Elf32_Sym *syms    = (Elf32_Sym *)(blob + symtab_sh->sh_offset);
    uint32_t   sym_cnt = symtab_sh->sh_size / sizeof(Elf32_Sym);

    for (uint32_t s = 0; s < sym_cnt; s++) {
        if (ELF32_ST_BIND(syms[s].st_info) != STB_GLOBAL) continue;
        if (ELF32_ST_TYPE(syms[s].st_info) != STT_FUNC)   continue;
        if (syms[s].st_shndx == SHN_UNDEF)                continue;
        const char *sym_name = get_str(eh, strtab_idx, syms[s].st_name);
        if (!sym_name) continue;
        if (strcmp((char *)sym_name, "fs_mount") == 0)
            return 1;
    }
    return 0;
}
