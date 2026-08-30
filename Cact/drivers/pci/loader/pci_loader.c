#include "pci_loader.h"
#include "pci_loader_internal.h"
#include "pci_driver.h"
#include "pci_enum.h"
#include "initfs_modblob.h"
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

int hmac_verify_module(uint8_t *elf_data, uint32_t *file_size) {
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
Elf32_Shdr *get_shdr(Elf32_Ehdr *eh, uint16_t idx) {
    if (idx >= eh->e_shnum)
        return NULL;
    return (Elf32_Shdr *)((uint8_t *)eh + eh->e_shoff + idx * eh->e_shentsize);
}

// Return string from string table 'strtab_idx' at offset 'off'
const char *get_str(Elf32_Ehdr *eh, uint16_t strtab_idx, uint32_t off) {
    Elf32_Shdr *sh = get_shdr(eh, strtab_idx);
    if (!sh)
        return NULL;
    return (const char *)((uint8_t *)eh + sh->sh_offset + off);
}

uint16_t read_le16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

// Read ET_REL file from VFS into kmalloc'd buffer; validate header.
// In-memory blobs (objcopy'd from LocalRepoCactOS/lib) take priority so that
// modules can load before VFS is up.
// Returns 0 on success; -1 open/read/alloc; -2 bad ELF.
int read_rel_elf_from_path(const char *path, uint8_t **elf_data, uint32_t *file_size) {
    const uint8_t *blob_data = NULL;
    uint32_t       blob_size = 0;

    if (initfs_modblob_get(path, &blob_data, &blob_size) == 0) {
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
