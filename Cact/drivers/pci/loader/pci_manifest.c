#include "pci_loader.h"
#include "pci_loader_internal.h"
#include "pci_driver.h"
#include "pci_enum.h"
#include "pci_modblob.h"
#include "cctkfs.h"
#include "vfs.h"
#include "memory.h"
#include "kernel.h"
#include "klib.h"
#include "ksym.h"

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
