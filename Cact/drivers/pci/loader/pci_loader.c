#include "pci_loader.h"
#include "pci_driver.h"
#include "pci_enum.h"
#include "pci_modblob.h"
#include "cctkfs.h"
#include "vfs.h"
#include "memory.h"
#include "kernel.h"
#include "klib.h"
#include "ksym.h"

// HMAC-SHA256 module signing — implemented in cact_crypto (Rust, no_std)
extern int  cact_hmac_verify(const uint8_t *data, uint32_t data_len,
                             const uint8_t *tag, uint32_t tag_len);

#define CACT_HMAC_TAG_SIZE 32

CACT_STATIC_ASSERT(CACT_HMAC_TAG_SIZE == 32);

// Wildcard ID — must match PCI_ANY_ID in pci_driver.h
#define LDR_PCI_ANY_ID 0xFFFFu

// ELF 32-bit types (i386)
typedef uint32_t Elf32_Addr;
typedef uint32_t Elf32_Off;
typedef uint16_t Elf32_Half;
typedef uint32_t Elf32_Word;
typedef  int32_t Elf32_Sword;

// ELF constants (relocatable object only)
#define ELF_MAGIC      0x464C457F
#define ET_REL         1           // relocatable object file
#define EM_386         3           // i386
#define SHT_PROGBITS   1
#define SHT_SYMTAB     2
#define SHT_STRTAB     3
#define SHT_REL        9
#define SHF_ALLOC      0x2
#define STB_GLOBAL     1
#define STB_WEAK       2
#define STT_NOTYPE     0
#define STT_OBJECT     1
#define STT_FUNC       2
#define SHN_UNDEF      0
#define R_386_32       1           // absolute relocation
#define R_386_PC32     2           // PC-relative relocation

// Accessor macros for ELF symbol info
#define ELF32_ST_BIND(i)  ((i) >> 4)
#define ELF32_ST_TYPE(i)  ((i) & 0xF)
#define ELF32_R_SYM(i)    ((i) >> 8)
#define ELF32_R_TYPE(i)   ((uint8_t)(i))

// ELF header — 52 bytes
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

// Section header — 40 bytes
typedef struct {
    Elf32_Word sh_name, sh_type, sh_flags;
    Elf32_Addr sh_addr;
    Elf32_Off  sh_offset;
    Elf32_Word sh_size, sh_link, sh_info, sh_addralign, sh_entsize;
} __attribute__((packed)) Elf32_Shdr;

// Symbol table entry — 16 bytes
typedef struct {
    Elf32_Word st_name;
    Elf32_Addr st_value;
    Elf32_Word st_size;
    uint8_t    st_info, st_other;
    Elf32_Half st_shndx;
} __attribute__((packed)) Elf32_Sym;

// Relocation entry (REL type) — 8 bytes
typedef struct {
    Elf32_Addr r_offset;
    Elf32_Word r_info;
} __attribute__((packed)) Elf32_Rel;

// Bookkeeping for a loaded module image
typedef struct {
    uint8_t  *image;
    uint32_t  image_size;
    char      proc_name[64];
} mod_mem_t;

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
        printk("[LDR] HMAC: unsigned module (no signature) — rejected\n");
        return -1;
    }
    uint32_t  data_len = *file_size - CACT_HMAC_TAG_SIZE;
    uint8_t  *tag      = elf_data + data_len;
    int       rc       = cact_hmac_verify(elf_data, data_len, tag, CACT_HMAC_TAG_SIZE);
    if (rc != 0) {
        printk("[LDR] HMAC: signature mismatch — rejected\n");
        return -1;
    }
    *file_size = data_len;
    // Zero the tag area so stray section-header reads past data_len
    // (e.g. the last .shstrtab entry that lands on the HMAC tag) do
    // not pick up garbage flags/alignment/sizes.
    for (uint32_t i = 0; i < CACT_HMAC_TAG_SIZE; i++)
        elf_data[data_len + i] = 0;
    return 0;
}

// Return pointer to section header 'idx'
static Elf32_Shdr *get_shdr(Elf32_Ehdr *eh, uint16_t idx) {
    if (idx >= eh->e_shnum)
        return NULL;
    return (Elf32_Shdr *)((uint8_t *)eh + eh->e_shoff + idx * eh->e_shentsize);
}

// Return string from string table 'strtab_idx' at offset 'off'
static const char *get_str(Elf32_Ehdr *eh, uint16_t strtab_idx, uint32_t off) {
    Elf32_Shdr *sh = get_shdr(eh, strtab_idx);
    if (!sh)
        return NULL;
    return (const char *)((uint8_t *)eh + sh->sh_offset + off);
}

static uint16_t read_le16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

// Read ET_REL file from VFS into kmalloc'd buffer; validate header.
// In-memory blobs (objcopy'd from LocalRepoCactOS/lib) take priority so that
// modules can load before VFS is up (used by GDD during PCI enumeration).
// Returns 0 on success; -1 open/read/alloc; -2 bad ELF.
static int read_rel_elf_from_path(const char *path, uint8_t **elf_data, uint32_t *file_size) {
    const uint8_t *blob_data = NULL;
    uint32_t       blob_size = 0;

    if (pci_modblob_get(path, &blob_data, &blob_size) == 0) {
        if (!blob_size)
            return -1;
        uint8_t *buf = (uint8_t *)kmalloc(blob_size);
        if (!buf)
            return -1;
        memcpy(buf, blob_data, blob_size);

        Elf32_Ehdr *eh = (Elf32_Ehdr *)buf;
        if (eh->e_magic != ELF_MAGIC || eh->e_type != ET_REL || eh->e_machine != EM_386) {
            kfree(buf);
            return -2;
        }
        *elf_data  = buf;
        *file_size = blob_size;
        return 0;
    }

    struct vfs_node *node = _resolve_path(path);
    if (!node)
        return -1;

    uint32_t fsz = node->size;
    if (!fsz) {
        vfs_node_unref(node);
        return -1;
    }

    uint8_t *buf = (uint8_t *)kmalloc(fsz);
    if (!buf) {
        vfs_node_unref(node);
        return -1;
    }

    int br = read_vfs(node, 0, fsz, (char *)buf);
    if (br <= 0) {
        kfree(buf);
        vfs_node_unref(node);
        return -1;
    }

    Elf32_Ehdr *eh = (Elf32_Ehdr *)buf;
    if (eh->e_magic != ELF_MAGIC || eh->e_type != ET_REL || eh->e_machine != EM_386) {
        kfree(buf);
        vfs_node_unref(node);
        return -2;
    }

    *elf_data   = buf;
    *file_size  = fsz;
    vfs_node_unref(node);
    return 0;
}

static int sym_is_data_global(const Elf32_Sym *sym) {
    if (ELF32_ST_BIND(sym->st_info) != STB_GLOBAL)
        return 0;
    uint8_t t = ELF32_ST_TYPE(sym->st_info);
    return (t == STT_OBJECT || t == STT_NOTYPE);
}

static Elf32_Sym *elf_find_named_sym(Elf32_Ehdr *eh, Elf32_Sym *syms, uint32_t sym_cnt,
                                     uint16_t strtab_idx, const char *want) {
    for (uint32_t s = 0; s < sym_cnt; s++) {
        if (!sym_is_data_global(&syms[s]))
            continue;
        const char *nm = get_str(eh, strtab_idx, syms[s].st_name);
        if (!nm) continue;
        if (strcmp((char *)nm, (char *)want) != 0)
            continue;
        return &syms[s];
    }
    return NULL;
}

static int read_sym_u16(Elf32_Ehdr *eh, const uint8_t *elf_data, const Elf32_Sym *sym,
                        uint16_t *out) {
    if (sym->st_shndx == SHN_UNDEF || sym->st_size < sizeof(uint16_t))
        return -1;
    Elf32_Shdr *sh = get_shdr(eh, sym->st_shndx);
    if (!sh || sh->sh_type != SHT_PROGBITS)
        return -1;
    const uint8_t *p = elf_data + sh->sh_offset + sym->st_value;
    *out             = read_le16(p);
    return 0;
}

static int read_sym_u8(Elf32_Ehdr *eh, const uint8_t *elf_data, const Elf32_Sym *sym,
                       uint8_t *out) {
    if (sym->st_shndx == SHN_UNDEF || sym->st_size < 1)
        return -1;
    Elf32_Shdr *sh = get_shdr(eh, sym->st_shndx);
    if (!sh || sh->sh_type != SHT_PROGBITS)
        return -1;
    *out = elf_data[sh->sh_offset + sym->st_value];
    return 0;
}

// Zero-terminated list of uint16 in .rodata/.data
static int read_device_id_list(Elf32_Ehdr *eh, const uint8_t *elf_data, Elf32_Sym *sym,
                               uint16_t *ids_out, int max_ids) {
    if (sym->st_shndx == SHN_UNDEF)
        return 0;
    Elf32_Shdr *sh = get_shdr(eh, sym->st_shndx);
    if (!sh || sh->sh_type != SHT_PROGBITS || sym->st_size < 2)
        return 0;
    const uint8_t *base = elf_data + sh->sh_offset + sym->st_value;
    uint32_t       nbytes = sym->st_size;
    int            n      = 0;
    for (uint32_t off = 0; off + 2 <= nbytes && n < max_ids; off += 2) {
        uint16_t v = read_le16(base + off);
        if (v == 0)
            break;
        ids_out[n++] = v;
    }
    return n;
}

static uint16_t choose_did_from_manifest(uint16_t vendor, const uint16_t *ids, int nids) {
    if (nids <= 0)
        return 0;
    for (pci_device_t *d = pci_device_list; d; d = d->next) {
        if (d->vendor_id != vendor)
            continue;
        for (int i = 0; i < nids; i++) {
            if (d->device_id == ids[i])
                return ids[i];
        }
    }
    return ids[0];
}

int pci_peek_module_manifest(const char *path, uint16_t *vendor_out, uint16_t *device_out,
                             uint8_t *class_out, uint8_t *subclass_out) {
    uint8_t *elf_data = NULL;
    uint32_t file_size = 0;
    int      rr        = read_rel_elf_from_path(path, &elf_data, &file_size);
    if (rr == -1) {
        printk("[LDR] manifest: file not found\n");
        return -1;
    }
    if (rr == -2) {
        printk("[LDR] manifest: not a valid ELF32 relocatable\n");
        return -2;
    }
    if (hmac_verify_module(elf_data, &file_size) != 0) {
        printk("[LDR] manifest: HMAC verification failed\n");
        kfree(elf_data);
        return -5;
    }

    Elf32_Ehdr *eh = (Elf32_Ehdr *)elf_data;

    // Validate section header table fits in file
    uint32_t sh_tab_end;
    if (__builtin_umul_overflow(eh->e_shnum, eh->e_shentsize, &sh_tab_end) ||
        __builtin_uadd_overflow(eh->e_shoff, sh_tab_end, &sh_tab_end) ||
        eh->e_shentsize < sizeof(Elf32_Shdr) || sh_tab_end > file_size) {
        printk("[LDR] manifest: corrupted section header table\n");
        kfree(elf_data);
        return -3;
    }

    Elf32_Shdr *symtab_sh = NULL;
    uint16_t    strtab_idx = 0;
    for (uint16_t i = 0; i < eh->e_shnum; i++) {
        Elf32_Shdr *sh = get_shdr(eh, i);
        if (sh->sh_type == SHT_SYMTAB) {
            symtab_sh  = sh;
            strtab_idx = (uint16_t)sh->sh_link;
            break;
        }
    }
    if (!symtab_sh) {
        printk("[LDR] manifest: no .symtab\n");
        kfree(elf_data);
        return -3;
    }

    // Validate symtab and its string table section data fit in file
    if (symtab_sh->sh_offset + symtab_sh->sh_size > file_size) {
        printk("[LDR] manifest: symtab exceeds file\n");
        kfree(elf_data);
        return -3;
    }
    Elf32_Shdr *strtab_sh = get_shdr(eh, strtab_idx);
    if (!strtab_sh || strtab_sh->sh_offset + strtab_sh->sh_size > file_size) {
        printk("[LDR] manifest: strtab exceeds file\n");
        kfree(elf_data);
        return -3;
    }

    Elf32_Sym *syms    = (Elf32_Sym *)(elf_data + symtab_sh->sh_offset);
    uint32_t   sym_cnt = symtab_sh->sh_size / sizeof(Elf32_Sym);

    // vendor + device are optional when class is set (class-bound drivers
    // such as AHCI/xHCI/NVMe do not need to enumerate VID/DID lists).
    uint16_t   vendor      = LDR_PCI_ANY_ID;
    int        have_vendor = 0;
    Elf32_Sym *sv = elf_find_named_sym(eh, syms, sym_cnt, strtab_idx, "cact_pci_vendor_id");
    if (sv && read_sym_u16(eh, elf_data, sv, &vendor) == 0)
        have_vendor = 1;

    uint16_t   ids[16];
    int        nids = 0;
    Elf32_Sym *slist = elf_find_named_sym(eh, syms, sym_cnt, strtab_idx, "cact_pci_device_ids");
    if (slist)
        nids = read_device_id_list(eh, elf_data, slist, ids, 16);

    if (nids == 0) {
        Elf32_Sym *sd =
            elf_find_named_sym(eh, syms, sym_cnt, strtab_idx, "cact_pci_device_id");
        if (sd && read_sym_u16(eh, elf_data, sd, &ids[0]) == 0)
            nids = 1;
    }

    Elf32_Sym *sc = elf_find_named_sym(eh, syms, sym_cnt, strtab_idx, "cact_pci_class");
    int have_class = 0;
    if (sc && read_sym_u8(eh, elf_data, sc, class_out) == 0)
        have_class = 1;
    Elf32_Sym *ss = elf_find_named_sym(eh, syms, sym_cnt, strtab_idx, "cact_pci_subclass");
    if (ss)
        (void)read_sym_u8(eh, elf_data, ss, subclass_out);

    if (!have_vendor && !have_class) {
        printk("[LDR] manifest: need cact_pci_vendor_id or cact_pci_class\n");
        kfree(elf_data);
        return -4;
    }

    uint16_t did;
    if (nids == 0) {
        did = LDR_PCI_ANY_ID;
    } else if (!have_vendor) {
        did = ids[0];
    } else {
        did = choose_did_from_manifest(vendor, ids, nids);
    }

    *vendor_out = vendor;
    *device_out = did;

    kfree(elf_data);
    return 0;
}

// Load a relocatable ELF module, relocate it into a private image,
// find the exported symbol "pci_driver_probe", and wire it into 'drv'.
int pci_load_module(const char *path, struct pci_driver *drv) {

    uint8_t *elf_data = NULL;
    uint32_t file_size = 0;
    int      rr        = read_rel_elf_from_path(path, &elf_data, &file_size);
    if (rr == -1) {
        printk("[LDR] File not found\n");
        return -1;
    }
    if (rr == -2) {
        printk("[LDR] Invalid ELF32 relocatable\n");
        return -2;
    }
    if (hmac_verify_module(elf_data, &file_size) != 0) {
        printk("[LDR] HMAC verification failed\n");
        kfree(elf_data);
        return -8;
    }

    Elf32_Ehdr *eh = (Elf32_Ehdr *)elf_data;

    // Validate section header table fits in file
    uint32_t sh_tab_end;
    if (__builtin_umul_overflow(eh->e_shnum, eh->e_shentsize, &sh_tab_end) ||
        __builtin_uadd_overflow(eh->e_shoff, sh_tab_end, &sh_tab_end) ||
        eh->e_shentsize < sizeof(Elf32_Shdr) || sh_tab_end > file_size) {
        printk("[LDR] corrupted module ELF\n");
        kfree(elf_data);
        return -8;
    }

    // First pass: calculate total image size and assign section addresses
    uint32_t total = 0;
    for (uint16_t i = 0; i < eh->e_shnum; i++) {
        Elf32_Shdr *sh = get_shdr(eh, i);
        if (!(sh->sh_flags & SHF_ALLOC)) continue;
        uint32_t align = sh->sh_addralign ? sh->sh_addralign : 1;
        if (align & (align - 1)) align = 1;
        total = (total + align - 1) & ~(align - 1);
        sh->sh_addr = total;
        total += sh->sh_size;
    }

    // Allocate and zero the module private image
    uint8_t *image = (uint8_t *)kmalloc(total);
    if (!image) { kfree(elf_data); return -3; }
    memset(image, 0, total);

    // Copy PROGBITS sections into image
    for (uint16_t i = 0; i < eh->e_shnum; i++) {
        Elf32_Shdr *sh = get_shdr(eh, i);
        if (!(sh->sh_flags & SHF_ALLOC) || sh->sh_type != SHT_PROGBITS) continue;
        if (sh->sh_offset + sh->sh_size > file_size) {
            printk("[LDR] section offset exceeds file\n");
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
        if (sh->sh_type == SHT_SYMTAB) {
            symtab_sh  = sh;
            strtab_idx = (uint16_t)sh->sh_link;
            break;
        }
    }
    if (!symtab_sh) {
        printk("[LDR] No .symtab found\n");
        kfree(image); kfree(elf_data);
        return -4;
    }

    if (symtab_sh->sh_offset + symtab_sh->sh_size > file_size) {
        printk("[LDR] symtab exceeds file\n");
        kfree(image); kfree(elf_data);
        return -8;
    }
    Elf32_Shdr *strtab_sh = get_shdr(eh, strtab_idx);
    if (!strtab_sh || strtab_sh->sh_offset + strtab_sh->sh_size > file_size) {
        printk("[LDR] strtab exceeds file\n");
        kfree(image); kfree(elf_data);
        return -8;
    }

    Elf32_Sym *syms    = (Elf32_Sym *)(elf_data + symtab_sh->sh_offset);
    uint32_t   sym_cnt = symtab_sh->sh_size / sizeof(Elf32_Sym);

    // Apply relocations (R_386_32 and R_386_PC32 only, intra-module)
    for (uint16_t i = 0; i < eh->e_shnum; i++) {
        Elf32_Shdr *sh = get_shdr(eh, i);
        if (sh->sh_type != SHT_REL) continue;
        Elf32_Shdr *target_sh = get_shdr(eh, sh->sh_info);
        if (!target_sh || !(target_sh->sh_flags & SHF_ALLOC)) continue;
        Elf32_Rel *rels    = (Elf32_Rel *)(elf_data + sh->sh_offset);
        uint32_t   rel_cnt = sh->sh_size / sizeof(Elf32_Rel);
        for (uint32_t r = 0; r < rel_cnt; r++) {
            uint32_t   sym_idx = ELF32_R_SYM(rels[r].r_info);
            if (sym_idx >= sym_cnt) {
                printk("[LDR] Relocation symbol index out of bounds\n");
                kfree(image);
                kfree(elf_data);
                return -7;
            }
            uint8_t    type    = ELF32_R_TYPE(rels[r].r_info);
            Elf32_Sym *sym     = &syms[sym_idx];
            uint32_t   S;

            if (sym->st_shndx == SHN_UNDEF) {
                const char *sym_name = get_str(eh, strtab_idx, sym->st_name);
                if (!sym_name) {
                    printk("[LDR] Bad string table index\n");
                    kfree(image);
                    kfree(elf_data);
                    return -7;
                }
                S = ksym_resolve(sym_name);
                if (S == 0 && ELF32_ST_BIND(sym->st_info) != STB_WEAK) {
                    printk("[LDR] Unresolved symbol: ");
                    printk((char *)sym_name);
                    printk("\n");
                    kfree(image);
                    kfree(elf_data);
                    return -7;
                }
            } else {
                Elf32_Shdr *sym_sh = get_shdr(eh, sym->st_shndx);
                if (!sym_sh) {
                    printk("[LDR] Symbol section index out of bounds\n");
                    kfree(image);
                    kfree(elf_data);
                    return -7;
                }
                if (!(sym_sh->sh_flags & SHF_ALLOC)) {
                    printk("[LDR] Symbol in non-ALLOC section\n");
                    kfree(image);
                    kfree(elf_data);
                    return -7;
                }
                S = (uint32_t)(image + sym_sh->sh_addr + sym->st_value);
            }
            if (rels[r].r_offset + sizeof(uint32_t) > target_sh->sh_size) {
                printk("[LDR] Relocation offset out of section bounds\n");
                kfree(image);
                kfree(elf_data);
                return -7;
            }
            uint32_t *patch = (uint32_t *)(image
                                + target_sh->sh_addr
                                + rels[r].r_offset);
            if      (type == R_386_32)   *patch += S;
            else if (type == R_386_PC32) *patch += S - (uint32_t)patch;
        }
    }

    // Find exported symbol "pci_driver_probe" (STB_GLOBAL, STT_FUNC)
    typedef int (*probe_fn_t)(pci_device_t *);
    probe_fn_t found_probe = NULL;

    for (uint32_t s = 0; s < sym_cnt; s++) {
        if (ELF32_ST_BIND(syms[s].st_info) != STB_GLOBAL) continue;
        if (ELF32_ST_TYPE(syms[s].st_info) != STT_FUNC)   continue;
        const char *sym_name = get_str(eh, strtab_idx, syms[s].st_name);
        if (!sym_name || strcmp((char *)sym_name, "pci_driver_probe") != 0) continue;
        if (syms[s].st_shndx == SHN_UNDEF) continue;
        Elf32_Shdr *sym_sh = get_shdr(eh, syms[s].st_shndx);
        if (!sym_sh) continue;
        found_probe = (probe_fn_t)(image + sym_sh->sh_addr + syms[s].st_value);
        break;
    }

    if (!found_probe) {
        printk("[LDR] Symbol 'pci_driver_probe' not found\n");
        kfree(image);
        kfree(elf_data);
        return -5;
    }

    drv->probe = found_probe;

    typedef void (*remove_fn_t)(pci_device_t *);
    remove_fn_t found_remove = NULL;
    for (uint32_t s = 0; s < sym_cnt; s++) {
        if (ELF32_ST_BIND(syms[s].st_info) != STB_GLOBAL) continue;
        if (ELF32_ST_TYPE(syms[s].st_info) != STT_FUNC)   continue;
        const char *sym_name = get_str(eh, strtab_idx, syms[s].st_name);
        if (!sym_name || strcmp((char *)sym_name, "pci_driver_remove") != 0) continue;
        if (syms[s].st_shndx == SHN_UNDEF) continue;
        Elf32_Shdr *sym_sh = get_shdr(eh, syms[s].st_shndx);
        if (!sym_sh) continue;
        found_remove = (remove_fn_t)(image + sym_sh->sh_addr + syms[s].st_value);
        break;
    }
    drv->remove = found_remove;

    // Bookkeeping: store image pointer so it can be freed on unload
    mod_mem_t *mm  = (mod_mem_t *)kmalloc(sizeof(mod_mem_t));
    if (!mm) {
        kfree(image);
        kfree(elf_data);
        drv->probe = NULL;
        return -6;
    }
    mm->image      = image;
    mm->image_size = total;
    module_proc_name(path, mm->proc_name, sizeof(mm->proc_name));
    drv->priv      = mm;
    drv->flags    |= PCI_DRV_F_RELOC_MODULE;

    kfree(elf_data);   // ELF header no longer needed
    printk("[LDR] module ready: "); printk(mm->proc_name);

    return 0;
}

// Unload a previously loaded module: call remove (if any), free image and bookkeeping
void pci_unload_module(struct pci_driver *drv) {
    if (!drv || !drv->priv) return;
    mod_mem_t *mm = (mod_mem_t *)drv->priv;
    if (drv->remove) drv->remove(NULL);
    kfree(mm->image);
    kfree(mm);
    drv->priv   = NULL;
    drv->probe  = NULL;
    drv->remove = NULL;
    drv->flags &= ~PCI_DRV_F_RELOC_MODULE;
    printk("[LDR] Module unloaded: "); printk(drv->name); printk("\n");
}