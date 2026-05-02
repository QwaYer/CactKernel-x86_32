#include "pci_loader.h"
#include "pci_driver.h"
#include "pci_enum.h"
#include "vfs.h"
#include "memory.h"
#include "kernel.h"
#include "klib.h"

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
#define STT_FUNC       2
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
} mod_mem_t;

// Return pointer to section header 'idx'
static Elf32_Shdr *get_shdr(Elf32_Ehdr *eh, uint16_t idx) {
    return (Elf32_Shdr *)((uint8_t *)eh + eh->e_shoff + idx * eh->e_shentsize);
}

// Return string from string table 'strtab_idx' at offset 'off'
static const char *get_str(Elf32_Ehdr *eh, uint16_t strtab_idx, uint32_t off) {
    Elf32_Shdr *sh = get_shdr(eh, strtab_idx);
    return (const char *)((uint8_t *)eh + sh->sh_offset + off);
}

// Walk a VFS path string and return the terminal node.
// Duplicated from syscall resolve logic to keep the loader self-contained.
static struct vfs_node *vfs_open_path(const char *path) {
    if (!vfs_root) return NULL;
    struct vfs_node *node = vfs_root;
    static char token[128];
    int ti = 0;
    const char *p = path;
    if (*p == '/') p++;
    while (*p) {
        if (*p == '/') {
            token[ti] = '\0';
            if (ti > 0) {
                node = finddir_vfs(node, token);
                if (!node) return NULL;
            }
            ti = 0;
        } else {
            if (ti < 127) token[ti++] = *p;
        }
        p++;
    }
    token[ti] = '\0';
    if (ti > 0) node = finddir_vfs(node, token);
    return node;
}

// Load a relocatable ELF module, relocate it into a private image,
// find the exported symbol "pci_driver_probe", and wire it into 'drv'.
int pci_load_module(const char *path, struct pci_driver *drv) {
    // Open file via VFS
    struct vfs_node *node = vfs_open_path(path);
    if (!node) {
        kprint("[LDR] File not found\n");
        return -1;
    }

    uint32_t file_size = node->size;
    if (!file_size) return -1;

    // Read entire file into heap
    uint8_t *elf_data = (uint8_t *)kmalloc(file_size);
    if (!elf_data) return -1;

    int bytes_read = read_vfs(node, 0, file_size, (char *)elf_data);
    if (bytes_read <= 0) {
        kprint("[LDR] read_vfs failed\n");
        kfree_heap(elf_data);
        return -1;
    }

    // Validate ELF header: magic, ET_REL, EM_386
    Elf32_Ehdr *eh = (Elf32_Ehdr *)elf_data;
    if (eh->e_magic != ELF_MAGIC || eh->e_type != ET_REL || eh->e_machine != EM_386) {
        kprint("[LDR] Invalid ELF32 relocatable\n");
        kfree_heap(elf_data);
        return -2;
    }

    // First pass: calculate total image size and assign section addresses
    uint32_t total = 0;
    for (uint16_t i = 0; i < eh->e_shnum; i++) {
        Elf32_Shdr *sh = get_shdr(eh, i);
        if (!(sh->sh_flags & SHF_ALLOC)) continue;
        uint32_t align = sh->sh_addralign ? sh->sh_addralign : 1;
        total = (total + align - 1) & ~(align - 1);
        sh->sh_addr = total;
        total += sh->sh_size;
    }

    // Allocate and zero the module private image
    uint8_t *image = (uint8_t *)kmalloc(total);
    if (!image) { kfree_heap(elf_data); return -3; }
    memset(image, 0, total);

    // Copy PROGBITS sections into image
    for (uint16_t i = 0; i < eh->e_shnum; i++) {
        Elf32_Shdr *sh = get_shdr(eh, i);
        if ((sh->sh_flags & SHF_ALLOC) && sh->sh_type == SHT_PROGBITS)
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
        kprint("[LDR] No .symtab found\n");
        kfree_heap(image); kfree_heap(elf_data);
        return -4;
    }

    Elf32_Sym *syms    = (Elf32_Sym *)(elf_data + symtab_sh->sh_offset);
    uint32_t   sym_cnt = symtab_sh->sh_size / sizeof(Elf32_Sym);

    // Apply relocations (R_386_32 and R_386_PC32 only, intra-module)
    for (uint16_t i = 0; i < eh->e_shnum; i++) {
        Elf32_Shdr *sh = get_shdr(eh, i);
        if (sh->sh_type != SHT_REL) continue;
        Elf32_Shdr *target_sh = get_shdr(eh, sh->sh_info);
        if (!(target_sh->sh_flags & SHF_ALLOC)) continue;
        Elf32_Rel *rels    = (Elf32_Rel *)(elf_data + sh->sh_offset);
        uint32_t   rel_cnt = sh->sh_size / sizeof(Elf32_Rel);
        for (uint32_t r = 0; r < rel_cnt; r++) {
            uint32_t   sym_idx = ELF32_R_SYM(rels[r].r_info);
            uint8_t    type    = ELF32_R_TYPE(rels[r].r_info);
            Elf32_Sym *sym     = &syms[sym_idx];
            uint32_t   S       = (uint32_t)(image
                                   + get_shdr(eh, sym->st_shndx)->sh_addr
                                   + sym->st_value);
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
        if (strcmp((char *)sym_name, "pci_driver_probe") != 0) continue;
        Elf32_Shdr *sym_sh = get_shdr(eh, syms[s].st_shndx);
        found_probe = (probe_fn_t)(image + sym_sh->sh_addr + syms[s].st_value);
        break;
    }

    kfree_heap(elf_data);   // ELF header no longer needed

    if (!found_probe) {
        kprint("[LDR] Symbol 'pci_driver_probe' not found\n");
        kfree_heap(image);
        return -5;
    }

    drv->probe = found_probe;

    // Bookkeeping: store image pointer so it can be freed on unload
    mod_mem_t *mm  = (mod_mem_t *)kmalloc(sizeof(mod_mem_t));
    mm->image      = image;
    mm->image_size = total;
    drv->priv      = mm;

    return 0;
}

// Unload a previously loaded module: call remove (if any), free image and bookkeeping
void pci_unload_module(struct pci_driver *drv) {
    if (!drv || !drv->priv) return;
    mod_mem_t *mm = (mod_mem_t *)drv->priv;
    if (drv->remove) drv->remove(NULL);
    kfree_heap(mm->image);
    kfree_heap(mm);
    drv->priv  = NULL;
    drv->probe = NULL;
    kprint("[LDR] Module unloaded: "); kprint(drv->name); kprint("\n");
}