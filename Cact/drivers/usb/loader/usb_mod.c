/*
 * Loadable USB driver-module loader (multi-slot).
 *
 * Mirrors fs_mod.c — the other non-PCI module class.  A USB driver module is
 * an ET_REL image (e.g. rt2800usb.cctk) that exports usb_driver_init(), which
 * typically calls the ksym-exported usb_driver_register() with a static
 * usb_driver_t.  The loader verifies the module's HMAC-SHA256 tag, relocates
 * it into a private image, resolves undefined symbols through ksym_resolve(),
 * and calls the entry point.
 */

#include "usb_mod.h"
#include "initfs_modblob.h"
#include "ksym.h"
#include "klib.h"
#include "memory.h"
#include "kernel.h"

// Linux i386 errno values, returned negated so the /dev/sys module ioctl can
// report a real reason (see kmod.c).
#ifndef EPERM
#define EPERM  1
#endif
#ifndef ENOENT
#define ENOENT 2
#endif
#ifndef ENOMEM
#define ENOMEM 12
#endif
#ifndef EACCES
#define EACCES 13
#endif
#ifndef EEXIST
#define EEXIST 17
#endif
#ifndef EINVAL
#define EINVAL 22
#endif
#ifndef ENOSPC
#define ENOSPC 28
#endif

// HMAC-SHA256 module signing — implemented in cact_crypto (Rust, no_std)
extern int cact_hmac_verify(const uint8_t *data, uint32_t data_len,
                            const uint8_t *tag, uint32_t tag_len);

#define CACT_HMAC_TAG_SIZE 32

// ELF 32-bit types (i386)
typedef uint32_t Elf32_Addr;
typedef uint32_t Elf32_Off;
typedef uint16_t Elf32_Half;
typedef uint32_t Elf32_Word;

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

typedef int (*usb_init_fn_t)(void);
typedef void (*usb_exit_fn_t)(void);

typedef struct {
    int          used;
    char         instance[64];
    uint8_t     *image;
    uint32_t     size;
    usb_exit_fn_t exit;
} usb_slot_t;

static usb_slot_t slots[USB_MOD_MAX];

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

// Instance name: path basename with a trailing ".cctk" stripped.
static void instance_name(const char *path, char *out, int out_sz) {
    const char *base = path;
    for (const char *p = path; p && *p; p++)
        if (*p == '/') base = p + 1;

    int i = 0;
    for (; base[i] && i < out_sz - 1; i++) {
        char c = base[i];
        if (c == ' ')
            c = '_';
        if (c == '.' && base[i + 1] == 'c' && base[i + 2] == 'c' &&
            base[i + 3] == 't' && base[i + 4] == 'k')
            break;
        out[i] = c;
    }
    out[i] = '\0';
}

static int hmac_verify_module(uint8_t *elf_data, uint32_t *file_size) {
    if (*file_size <= CACT_HMAC_TAG_SIZE) {
        pr_err("[USBMOD] HMAC: unsigned module (no signature) — rejected\n");
        return -1;
    }
    uint32_t data_len = *file_size - CACT_HMAC_TAG_SIZE;
    uint8_t *tag      = elf_data + data_len;
    if (cact_hmac_verify(elf_data, data_len, tag, CACT_HMAC_TAG_SIZE) != 0) {
        pr_err("[USBMOD] HMAC: signature mismatch — rejected\n");
        return -1;
    }
    *file_size = data_len;
    for (uint32_t i = 0; i < CACT_HMAC_TAG_SIZE; i++)
        elf_data[data_len + i] = 0;
    return 0;
}

// Locate an exported global function symbol by name.  Returns 0 if absent.
static uint32_t find_func_sym(Elf32_Ehdr *eh, Elf32_Sym *syms, uint32_t sym_cnt,
                              uint16_t strtab_idx, uint8_t *image,
                              const char *want) {
    for (uint32_t s = 0; s < sym_cnt; s++) {
        if (ELF32_ST_BIND(syms[s].st_info) != STB_GLOBAL) continue;
        if (ELF32_ST_TYPE(syms[s].st_info) != STT_FUNC)   continue;
        if (syms[s].st_shndx == SHN_UNDEF)                continue;
        const char *sym_name = get_str(eh, strtab_idx, syms[s].st_name);
        if (!sym_name || strcmp((char *)sym_name, (char *)want) != 0) continue;
        Elf32_Shdr *sym_sh = get_shdr(eh, syms[s].st_shndx);
        if (!sym_sh) continue;
        return (uint32_t)(image + sym_sh->sh_addr + syms[s].st_value);
    }
    return 0;
}

// Load, verify and relocate the module; resolve usb_driver_init/_exit.
static int load_module_image(const char *path, uint8_t **image_out,
                             uint32_t *size_out, usb_init_fn_t *init_out,
                             usb_exit_fn_t *exit_out) {
    const uint8_t *blob_data = NULL;
    uint32_t       blob_size = 0;
    if (initfs_modblob_get(path, &blob_data, &blob_size) != 0 || !blob_size) {
        pr_err("[USBMOD] module not found: %s\n", path);
        return -ENOENT;
    }

    uint8_t *elf_data = (uint8_t *)kmalloc(blob_size);
    if (!elf_data) return -ENOMEM;
    memcpy(elf_data, blob_data, blob_size);
    uint32_t file_size = blob_size;

    if (hmac_verify_module(elf_data, &file_size) != 0) {
        kfree(elf_data);
        return -EACCES;
    }

    Elf32_Ehdr *eh = (Elf32_Ehdr *)elf_data;
    if (eh->e_magic != ELF_MAGIC || eh->e_type != ET_REL || eh->e_machine != EM_386) {
        pr_err("[USBMOD] not a valid ELF32 relocatable\n");
        kfree(elf_data);
        return -2;
    }

    uint32_t sh_tab_end;
    if (__builtin_umul_overflow(eh->e_shnum, eh->e_shentsize, &sh_tab_end) ||
        __builtin_uadd_overflow(eh->e_shoff, sh_tab_end, &sh_tab_end) ||
        eh->e_shentsize < sizeof(Elf32_Shdr) || sh_tab_end > file_size) {
        pr_err("[USBMOD] corrupted section header table\n");
        kfree(elf_data);
        return -3;
    }

    // First pass: total image size and section addresses.
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
    if (!image) { kfree(elf_data); return -ENOMEM; }
    memset(image, 0, total);

    for (uint16_t i = 0; i < eh->e_shnum; i++) {
        Elf32_Shdr *sh = get_shdr(eh, i);
        if (!sh || !(sh->sh_flags & SHF_ALLOC) || sh->sh_type != SHT_PROGBITS) continue;
        if (sh->sh_offset + sh->sh_size > file_size) {
            pr_err("[USBMOD] section offset exceeds file\n");
            kfree(image); kfree(elf_data);
            return -8;
        }
        memcpy(image + sh->sh_addr, elf_data + sh->sh_offset, sh->sh_size);
    }

    // Symbol table + its string table.
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
        pr_err("[USBMOD] no .symtab found\n");
        kfree(image); kfree(elf_data);
        return -4;
    }
    if (symtab_sh->sh_offset + symtab_sh->sh_size > file_size) {
        kfree(image); kfree(elf_data);
        return -8;
    }
    Elf32_Shdr *strtab_sh = get_shdr(eh, strtab_idx);
    if (!strtab_sh || strtab_sh->sh_offset + strtab_sh->sh_size > file_size) {
        kfree(image); kfree(elf_data);
        return -8;
    }
    Elf32_Sym *syms    = (Elf32_Sym *)(elf_data + symtab_sh->sh_offset);
    uint32_t   sym_cnt = symtab_sh->sh_size / sizeof(Elf32_Sym);

    // Relocations (R_386_32 / R_386_PC32), intra-module + ksym.
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
                pr_err("[USBMOD] relocation symbol index out of bounds\n");
                kfree(image); kfree(elf_data);
                return -7;
            }
            uint8_t    type = ELF32_R_TYPE(rels[r].r_info);
            Elf32_Sym *sym  = &syms[sym_idx];
            uint32_t   S;
            if (sym->st_shndx == SHN_UNDEF) {
                const char *sym_name = get_str(eh, strtab_idx, sym->st_name);
                if (!sym_name) { kfree(image); kfree(elf_data); return -7; }
                S = ksym_resolve(sym_name);
                if (S == 0 && ELF32_ST_BIND(sym->st_info) != STB_WEAK) {
                    pr_err("[USBMOD] unresolved symbol: %s\n", sym_name);
                    kfree(image); kfree(elf_data);
                    return -7;
                }
            } else {
                Elf32_Shdr *sym_sh = get_shdr(eh, sym->st_shndx);
                if (!sym_sh || !(sym_sh->sh_flags & SHF_ALLOC)) {
                    pr_err("[USBMOD] bad symbol section\n");
                    kfree(image); kfree(elf_data);
                    return -7;
                }
                S = (uint32_t)(image + sym_sh->sh_addr + sym->st_value);
            }
            if (rels[r].r_offset + sizeof(uint32_t) > target_sh->sh_size) {
                pr_err("[USBMOD] relocation offset out of bounds\n");
                kfree(image); kfree(elf_data);
                return -7;
            }
            uint32_t *patch = (uint32_t *)(image + target_sh->sh_addr + rels[r].r_offset);
            if      (type == R_386_32)   *patch += S;
            else if (type == R_386_PC32) *patch += S - (uint32_t)patch;
        }
    }

    uint32_t init_addr = find_func_sym(eh, syms, sym_cnt, strtab_idx, image,
                                       "usb_driver_init");
    if (!init_addr) {
        pr_err("[USBMOD] symbol 'usb_driver_init' not found\n");
        kfree(image); kfree(elf_data);
        return -5;
    }
    uint32_t exit_addr = find_func_sym(eh, syms, sym_cnt, strtab_idx, image,
                                       "usb_driver_exit");

    kfree(elf_data);
    *image_out = image;
    *size_out  = total;
    *init_out  = (usb_init_fn_t)init_addr;
    *exit_out  = exit_addr ? (usb_exit_fn_t)exit_addr : NULL;
    return 0;
}

static int usb_mod_image_errno(int rc) {
    switch (rc) {
    case -ENOENT: return -ENOENT;   // no such module in the cctkfs image
    case -ENOMEM: return -ENOMEM;   // relocation image too large
    case -EACCES: return -EACCES;   // HMAC signature rejected
    default:      return -EINVAL;   // not ET_REL / corrupt / unresolved symbol
    }
}

int usb_mod_load(const char *path) {
    if (!path) return -EINVAL;

    int free_slot = -1;
    for (int i = 0; i < USB_MOD_MAX; i++)
        if (!slots[i].used) { free_slot = i; break; }
    if (free_slot < 0) {
        pr_err("[USBMOD] no free USB-module slot\n");
        return -ENOSPC;
    }

    char inst[64];
    instance_name(path, inst, sizeof(inst));
    for (int i = 0; i < USB_MOD_MAX; i++) {
        if (slots[i].used && strcmp(slots[i].instance, inst) == 0) {
            pr_warn("[USBMOD] module already loaded: %s\n", inst);
            return -EEXIST;
        }
    }

    uint8_t      *image = NULL;
    uint32_t      size  = 0;
    usb_init_fn_t init  = NULL;
    usb_exit_fn_t exit  = NULL;
    int rc = load_module_image(path, &image, &size, &init, &exit);
    if (rc != 0)
        return usb_mod_image_errno(rc);

    // Publish the slot before calling init: init registers drivers whose
    // probe may fire immediately, and those must see the module as resident.
    usb_slot_t *s = &slots[free_slot];
    memset(s, 0, sizeof(*s));
    strlcpy(s->instance, inst, sizeof(s->instance));
    s->used  = 1;
    s->image = image;
    s->size  = size;
    s->exit  = exit;

    int irc = init();
    if (irc != 0) {
        pr_err("[USBMOD] usb_driver_init failed: %d\n", irc);
        memset(s, 0, sizeof(*s));
        kfree(image);
        return irc;
    }

    pr_info("[USBMOD] module loaded: %s (slot %d)\n", inst, free_slot);
    return 0;
}

static usb_slot_t *slot_by_instance(const char *instance) {
    if (!instance)
        return 0;
    for (int i = 0; i < USB_MOD_MAX; i++)
        if (slots[i].used && strcmp(slots[i].instance, instance) == 0)
            return &slots[i];
    return 0;
}

int usb_mod_unload(const char *instance) {
    usb_slot_t *s = slot_by_instance(instance);
    if (!s)
        return -ENOENT;
    if (s->exit)
        s->exit();
    if (s->image)
        kfree(s->image);
    memset(s, 0, sizeof(*s));
    pr_info("[USBMOD] module unloaded: %s\n", instance);
    return 0;
}

int usb_mod_loaded(const char *instance) {
    return slot_by_instance(instance) != 0;
}

int usb_mod_count(void) {
    int n = 0;
    for (int i = 0; i < USB_MOD_MAX; i++)
        if (slots[i].used) n++;
    return n;
}

const char *usb_mod_instance(int slot) {
    if (slot < 0 || slot >= USB_MOD_MAX || !slots[slot].used)
        return 0;
    return slots[slot].instance;
}

// Non-destructive probe: scan the ELF symbol table for an exported global FUNC
// named 'usb_driver_init'.  Used by modload to route a .cctk to this loader.
int usb_mod_detect(const char *path) {
    if (!path) return -1;

    const uint8_t *blob = NULL;
    uint32_t       size = 0;
    if (initfs_modblob_get(path, &blob, &size) != 0 || !blob || !size)
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
        if (sym_name && strcmp((char *)sym_name, "usb_driver_init") == 0)
            return 1;
    }
    return 0;
}
