#ifndef PCI_LOADER_INTERNAL_H
#define PCI_LOADER_INTERNAL_H

#include <stdint.h>

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

// pci_loader.c — file/section readers shared with the manifest pass.
int  hmac_verify_module(uint8_t *elf_data, uint32_t *file_size);
int  read_rel_elf_from_path(const char *path, uint8_t **elf_data, uint32_t *file_size);
Elf32_Shdr *get_shdr(Elf32_Ehdr *eh, uint16_t idx);
const char *get_str(Elf32_Ehdr *eh, uint16_t strtab_idx, uint32_t off);
uint16_t read_le16(const uint8_t *p);

#endif
