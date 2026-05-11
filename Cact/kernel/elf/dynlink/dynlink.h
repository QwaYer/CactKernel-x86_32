#ifndef DYNLINK_H
#define DYNLINK_H

#include <stdint.h>
#include "elf.h"
#include "memory.h"
#include "proc_mm.h"


typedef struct {
    Elf32_Sword d_tag;
    union {
        Elf32_Word d_val;
        Elf32_Addr d_ptr;
    } d_un;
} Elf32_Dyn;

typedef struct {
    Elf32_Word st_name;
    Elf32_Addr st_value;
    Elf32_Word st_size;
    uint8_t    st_info;
    uint8_t    st_other;
    Elf32_Half st_shndx;
} Elf32_Sym;

typedef struct {
    Elf32_Addr r_offset;
    Elf32_Word r_info;
} Elf32_Rel;

typedef struct {
    Elf32_Addr  r_offset;
    Elf32_Word  r_info;
    Elf32_Sword r_addend;
} Elf32_Rela;

#define DT_NULL     0
#define DT_NEEDED   1
#define DT_PLTRELSZ 2
#define DT_PLTGOT   3
#define DT_HASH     4
#define DT_STRTAB   5
#define DT_SYMTAB   6
#define DT_RELA     7
#define DT_RELASZ   8
#define DT_RELAENT  9
#define DT_STRSZ    10
#define DT_SYMENT   11
#define DT_INIT     12
#define DT_FINI     13
#define DT_SONAME   14
#define DT_RPATH    15
#define DT_REL      17
#define DT_RELSZ    18
#define DT_RELENT   19
#define DT_PLTREL   20
#define DT_JMPREL   23
#define DT_GNU_HASH 0x6ffffef5

#define STB_LOCAL   0
#define STB_GLOBAL  1
#define STB_WEAK    2

#define STT_NOTYPE  0
#define STT_OBJECT  1
#define STT_FUNC    2

#define SHN_UNDEF   0
#define SHN_ABS     0xFFF1
#define SHN_COMMON  0xFFF2

#define ELF32_ST_BIND(i)    ((i) >> 4)
#define ELF32_ST_TYPE(i)    ((i) & 0xF)
#define ELF32_R_SYM(i)      ((i) >> 8)
#define ELF32_R_TYPE(i)     ((uint8_t)(i))

#define R_386_NONE      0
#define R_386_32        1   // S + A 
#define R_386_PC32      2   // S + A - P 
#define R_386_GOT32     3
#define R_386_PLT32     4
#define R_386_COPY      5
#define R_386_GLOB_DAT  6   // S 
#define R_386_JMP_SLOT  7   // S 
#define R_386_RELATIVE  8   // B + A 
#define R_386_GOTOFF    9
#define R_386_GOTPC     10


#define SO_NAME_MAX   128
#define SO_DEPS_MAX   16
#define SO_TABLE_MAX  32 

typedef struct loaded_so {
    char     name[SO_NAME_MAX];  
    uint32_t load_base;           
    uint32_t load_size;          

    Elf32_Sym* symtab;           
    uint32_t   symtab_count;      
    char*      strtab;         

    uint32_t   init_addr;     
    uint32_t   fini_addr;        

    int        ref_count;         
} loaded_so_t;


typedef struct {
    uint32_t*          pd;                    
    proc_page_tracker_t* tracker;            

    loaded_so_t        table[SO_TABLE_MAX];    
    int                count;                   

    char               so_search_path[256];     // Default: "/lib:/usr/lib"  
} dyn_ctx_t;


//Public api
void dynlink_ctx_init(dyn_ctx_t* ctx, uint32_t* pd,
                      proc_page_tracker_t* tracker);

loaded_so_t* dynlink_load_so(dyn_ctx_t* ctx, const char* name);

/*
 * image_start: load address of the object (lowest mapped PT_LOAD vaddr): used as B in
 *              R_386_RELATIVE and to key the <main> resolver slot.
 * sym_bias:    added to st_value for defined symbols in *this* object (image_start for
 *              ET_DYN/PIE, 0 for ET_EXEC where st_value is already absolute).
 */
int dynlink_process_dynamic(dyn_ctx_t* ctx, uint32_t image_start, uint32_t sym_bias,
                             Elf32_Dyn* dyn);

uint32_t dynlink_resolve_symbol(dyn_ctx_t* ctx, const char* name);

void dynlink_unload_all(dyn_ctx_t* ctx);
dyn_ctx_t* dynlink_ctx_create(uint32_t* pd, proc_page_tracker_t* tracker);
void dynlink_ctx_destroy(dyn_ctx_t* ctx);

/*
 * load_elf_dynamic lives in elf_loader.c but takes a dyn_ctx_t*, so its
 * prototype is exposed here next to the dynlink types it depends on.
 * The plain (non-dynamic) loader is declared in elf_loader.c — callers that
 * need it can declare it locally; only the dynamic path is widely used.
 */
void* load_elf_dynamic(char* path, uint32_t* pd,
                       proc_page_tracker_t* tracker, dyn_ctx_t* ctx);

#endif