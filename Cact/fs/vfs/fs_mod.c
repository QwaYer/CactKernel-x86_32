/*
 * Loadable filesystem-module loader (multi-slot).
 *
 * Mirrors pci_load_module() but for non-PCI filesystems: it loads an ET_REL
 * module (typically `ext4.cctk`) from the staged cctkfs image, verifies its
 * HMAC-SHA256 tag, relocates it, and resolves the generic `fs_mount` /
 * `fs_unmount` entry symbols.  Up to FS_MOD_MAX modules can be resident at
 * once; mntfs and the /dev/sys mount ioctl select a module by instance name
 * or probe them in registration order.
 *
 * Undefined kernel symbols referenced by the module are resolved through
 * ksym_resolve(), exactly like PCI modules do.
 */

#include "fs_mod.h"
#include "initfs_modblob.h"
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

// Resident module slot
typedef struct {
    int             used;
    char            instance[64];
    uint8_t        *image;
    uint32_t        size;
    fs_mount_fn_t   mount;
    fs_unmount_fn_t unmount;

    char            mounted[FS_MOD_MAX_MOUNTS][BLKDEV_NAME_MAX];
    uint32_t        mount_count;
} fs_slot_t;

static fs_slot_t slots[FS_MOD_MAX];

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
        // strip ".cctk" / ".so" style suffix
        if (c == '.' &&
            (base[i+1] == 'c' || base[i+1] == 's') &&
            base[i+2] == 'c' && base[i+3] == 't' && base[i+4] == 'k')
            break;
        out[i] = c;
    }
    out[i] = '\0';
}

static int hmac_verify_module(uint8_t *elf_data, uint32_t *file_size) {
    if (*file_size <= CACT_HMAC_TAG_SIZE) {
        printk("[FSMOD] HMAC: unsigned module (no signature) — rejected\n");
        return -1;
    }
    uint32_t  data_len = *file_size - CACT_HMAC_TAG_SIZE;
    uint8_t  *tag      = elf_data + data_len;
    int       rc       = cact_hmac_verify(elf_data, data_len, tag, CACT_HMAC_TAG_SIZE);
    if (rc != 0) {
        printk("[FSMOD] HMAC: signature mismatch — rejected\n");
        return -1;
    }
    *file_size = data_len;
    for (uint32_t i = 0; i < CACT_HMAC_TAG_SIZE; i++)
        elf_data[data_len + i] = 0;
    return 0;
}

// Shared relocation/entry-point lookup. Returns a freshly kmalloc'd image
// (must be freed by caller on error) or NULL.
static int load_module_image(const char *path, uint8_t **image_out,
                             uint32_t *size_out, fs_mount_fn_t *mount_out,
                             fs_unmount_fn_t *unmount_out) {
    const uint8_t *blob_data = NULL;
    uint32_t       blob_size = 0;
    if (initfs_modblob_get(path, &blob_data, &blob_size) != 0 || !blob_size) {
        printk("[FSMOD] module not found: "); printk((char *)path); printk("\n");
        return -1;
    }

    uint8_t *elf_data = (uint8_t *)kmalloc(blob_size);
    if (!elf_data) return -1;
    memcpy(elf_data, blob_data, blob_size);
    uint32_t file_size = blob_size;

    if (hmac_verify_module(elf_data, &file_size) != 0) {
        kfree(elf_data);
        return -8;
    }

    Elf32_Ehdr *eh = (Elf32_Ehdr *)elf_data;
    if (eh->e_magic != ELF_MAGIC || eh->e_type != ET_REL || eh->e_machine != EM_386) {
        printk("[FSMOD] not a valid ELF32 relocatable\n");
        kfree(elf_data);
        return -2;
    }

    uint32_t sh_tab_end;
    if (__builtin_umul_overflow(eh->e_shnum, eh->e_shentsize, &sh_tab_end) ||
        __builtin_uadd_overflow(eh->e_shoff, sh_tab_end, &sh_tab_end) ||
        eh->e_shentsize < sizeof(Elf32_Shdr) || sh_tab_end > file_size) {
        printk("[FSMOD] corrupted section header table\n");
        kfree(elf_data);
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
    if (!image) { kfree(elf_data); return -3; }
    memset(image, 0, total);

    // Copy PROGBITS sections into image
    for (uint16_t i = 0; i < eh->e_shnum; i++) {
        Elf32_Shdr *sh = get_shdr(eh, i);
        if (!sh || !(sh->sh_flags & SHF_ALLOC) || sh->sh_type != SHT_PROGBITS) continue;
        if (sh->sh_offset + sh->sh_size > file_size) {
            printk("[FSMOD] section offset exceeds file\n");
            kfree(image); kfree(elf_data);
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
        printk("[FSMOD] no .symtab found\n");
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
                printk("[FSMOD] relocation symbol index out of bounds\n");
                kfree(image); kfree(elf_data);
                return -7;
            }
            uint8_t    type    = ELF32_R_TYPE(rels[r].r_info);
            Elf32_Sym *sym     = &syms[sym_idx];
            uint32_t   S;
            if (sym->st_shndx == SHN_UNDEF) {
                const char *sym_name = get_str(eh, strtab_idx, sym->st_name);
                if (!sym_name) { kfree(image); kfree(elf_data); return -7; }
                S = ksym_resolve(sym_name);
                if (S == 0 && ELF32_ST_BIND(sym->st_info) != STB_WEAK) {
                    printk("[FSMOD] unresolved symbol: "); printk((char *)sym_name); printk("\n");
                    kfree(image); kfree(elf_data);
                    return -7;
                }
            } else {
                Elf32_Shdr *sym_sh = get_shdr(eh, sym->st_shndx);
                if (!sym_sh || !(sym_sh->sh_flags & SHF_ALLOC)) {
                    printk("[FSMOD] bad symbol section\n");
                    kfree(image); kfree(elf_data);
                    return -7;
                }
                S = (uint32_t)(image + sym_sh->sh_addr + sym->st_value);
            }
            if (rels[r].r_offset + sizeof(uint32_t) > target_sh->sh_size) {
                printk("[FSMOD] relocation offset out of bounds\n");
                kfree(image); kfree(elf_data);
                return -7;
            }
            uint32_t *patch = (uint32_t *)(image + target_sh->sh_addr + rels[r].r_offset);
            if      (type == R_386_32)   *patch += S;
            else if (type == R_386_PC32) *patch += S - (uint32_t)patch;
        }
    }

    // Find exported symbols "fs_mount" (required) and "fs_unmount" (optional)
    fs_mount_fn_t   mnt = NULL;
    fs_unmount_fn_t unm = NULL;
    for (uint32_t s = 0; s < sym_cnt; s++) {
        if (ELF32_ST_BIND(syms[s].st_info) != STB_GLOBAL) continue;
        if (ELF32_ST_TYPE(syms[s].st_info) != STT_FUNC)   continue;
        if (syms[s].st_shndx == SHN_UNDEF) continue;
        const char *sym_name = get_str(eh, strtab_idx, syms[s].st_name);
        if (!sym_name) continue;
        Elf32_Shdr *sym_sh = get_shdr(eh, syms[s].st_shndx);
        if (!sym_sh) continue;
        uint32_t addr = (uint32_t)(image + sym_sh->sh_addr + syms[s].st_value);
        if (strcmp((char *)sym_name, "fs_mount") == 0)
            mnt = (fs_mount_fn_t)addr;
        else if (strcmp((char *)sym_name, "fs_unmount") == 0)
            unm = (fs_unmount_fn_t)addr;
    }
    if (!mnt) {
        printk("[FSMOD] symbol 'fs_mount' not found\n");
        kfree(image); kfree(elf_data);
        return -5;
    }

    kfree(elf_data);
    *image_out  = image;
    *size_out   = total;
    *mount_out  = mnt;
    *unmount_out = unm;
    return 0;
}

int fs_mod_load(const char *path) {
    if (!path) return -1;

    int free_slot = -1;
    for (int i = 0; i < FS_MOD_MAX; i++) {
        if (!slots[i].used) {
            if (free_slot < 0) free_slot = i;
            continue;
        }
    }
    if (free_slot < 0) {
        printk("[FSMOD] no free filesystem-module slot\n");
        return -1;
    }

    char inst[64];
    instance_name(path, inst, sizeof(inst));
    for (int i = 0; i < FS_MOD_MAX; i++) {
        if (slots[i].used && strcmp(slots[i].instance, inst) == 0) {
            printk("[FSMOD] filesystem module already loaded: "); printk(inst); printk("\n");
            return -1;
        }
    }

    uint8_t        *image = NULL;
    uint32_t        size  = 0;
    fs_mount_fn_t   mnt   = NULL;
    fs_unmount_fn_t unm   = NULL;
    int rc = load_module_image(path, &image, &size, &mnt, &unm);
    if (rc != 0)
        return rc;

    fs_slot_t *s = &slots[free_slot];
    memset(s, 0, sizeof(*s));
    strlcpy(s->instance, inst, sizeof(s->instance));
    s->used    = 1;
    s->image   = image;
    s->size    = size;
    s->mount   = mnt;
    s->unmount = unm;
    s->mount_count = 0;

    printk("[FSMOD] module loaded: "); printk(inst);
    printk(" (slot "); { char b[8]; snprintf(b, sizeof(b), "%d", free_slot); printk(b); }
    printk(")\n");
    return 0;
}

static fs_slot_t *slot_by_instance(const char *instance) {
    if (!instance)
        return 0;
    for (int i = 0; i < FS_MOD_MAX; i++)
        if (slots[i].used && strcmp(slots[i].instance, (char *)instance) == 0)
            return &slots[i];
    return 0;
}

static int slot_unload(fs_slot_t *s) {
    if (!s || !s->used)
        return -1;
    if (s->mount_count > 0) {
        printk("[FSMOD] module busy (still mounted), not unloading: ");
        printk(s->instance);
        printk("\n");
        return -1;
    }
    if (s->image)
        kfree(s->image);
    memset(s, 0, sizeof(*s));
    return 0;
}

int fs_mod_unload_slot(int slot) {
    if (slot < 0 || slot >= FS_MOD_MAX)
        return -1;
    return slot_unload(&slots[slot]);
}

int fs_mod_unload(const char *instance) {
    fs_slot_t *s = slot_by_instance(instance);
    if (!s)
        return -1;
    int rc = slot_unload(s);
    if (rc == 0) {
        printk("[FSMOD] module unloaded: ");
        printk((char *)instance);
        printk("\n");
    }
    return rc;
}

int fs_mod_count(void) {
    int n = 0;
    for (int i = 0; i < FS_MOD_MAX; i++)
        if (slots[i].used) n++;
    return n;
}

int fs_mod_loaded_any(void) {
    return fs_mod_count() > 0;
}

int fs_mod_loaded(const char *instance) {
    return slot_by_instance(instance) != 0;
}

const char *fs_mod_instance(int slot) {
    if (slot < 0 || slot >= FS_MOD_MAX || !slots[slot].used)
        return 0;
    return slots[slot].instance;
}

// Remember a successful mount so an unload can be refused while in use.
static void slot_note_mount(fs_slot_t *s, const char *devname) {
    if (!s || !devname)
        return;
    for (uint32_t i = 0; i < s->mount_count; i++)
        if (strcmp(s->mounted[i], (char *)devname) == 0)
            return;
    if (s->mount_count >= FS_MOD_MAX_MOUNTS)
        return;
    strlcpy(s->mounted[s->mount_count], devname, BLKDEV_NAME_MAX);
    s->mount_count++;
}

vfs_node_t *fs_mod_mount_type(blkdev_t *dev, const char *fstype) {
    if (!dev)
        return NULL;

    if (!fstype || fstype[0] == '\0' ||
        strcmp((char *)fstype, "auto") == 0 || strcmp((char *)fstype, "*") == 0)
        return fs_mod_mount(dev);

    fs_slot_t *s = slot_by_instance(fstype);
    if (!s)
        return NULL;
    vfs_node_t *root = s->mount ? s->mount(dev) : NULL;
    if (root)
        slot_note_mount(s, dev->name);
    return root;
}

vfs_node_t *fs_mod_mount(blkdev_t *dev) {
    if (!dev)
        return NULL;
    for (int i = 0; i < FS_MOD_MAX; i++) {
        fs_slot_t *s = &slots[i];
        if (!s->used || !s->mount)
            continue;
        vfs_node_t *root = s->mount(dev);
        if (root) {
            slot_note_mount(s, dev->name);
            return root;
        }
    }
    return NULL;
}

int fs_mod_unmount_dev(blkdev_t *dev) {
    if (!dev)
        return -1;
    int rc = -1;
    for (int i = 0; i < FS_MOD_MAX; i++) {
        fs_slot_t *s = &slots[i];
        if (!s->used)
            continue;
        int removed = 0;
        for (uint32_t m = 0; m < s->mount_count; m++) {
            if (strcmp(s->mounted[m], (char *)dev->name) == 0) {
                // compact
                for (uint32_t k = m; k + 1 < s->mount_count; k++)
                    strlcpy(s->mounted[k], s->mounted[k + 1], BLKDEV_NAME_MAX);
                s->mount_count--;
                removed = 1;
                break;
            }
        }
        if (removed) {
            if (s->unmount)
                s->unmount();
            rc = 0;
        }
    }
    return rc;
}

// Non-destructive probe: scan the ELF symbol table of the module at 'path'
// for an exported global FUNC named 'fs_mount'. Used to auto-detect whether
// modload should treat a .cctk as a filesystem module.
int fs_mod_detect(const char *path) {
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
        if (!sym_name) continue;
        if (strcmp((char *)sym_name, "fs_mount") == 0)
            return 1;
    }
    return 0;
}
