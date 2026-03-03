#include "dynlink.h"
#include "vfs.h"
#include "memory.h"
#include "libc.h"
#include "kernel.h"


static int _so_strcmp(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

static int _so_strlen(const char* s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

static void _so_strncpy(char* dst, const char* src, int n) {
    int i = 0;
    while (src[i] && i < n - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static void _so_path_join(char* dst, int dst_sz,
                           const char* dir, const char* name) {
    int dl = _so_strlen(dir);
    int nl = _so_strlen(name);
    if (dl + nl + 2 > dst_sz) return;
    _so_strncpy(dst, dir, dst_sz);
    dst[dl]     = '/';
    dst[dl + 1] = '\0';
    _so_strncpy(dst + dl + 1, name, dst_sz - dl - 1);
}

static uint32_t _elf_map_file(struct vfs_node* file,
                               uint32_t*        pd,
                               proc_page_tracker_t* tracker,
                               uint32_t*        out_entry,
                               Elf32_Dyn**      out_dyn,
                               uint32_t*        out_dyn_size)
{
    Elf32_Ehdr hdr;
    if (read_vfs(file, 0, sizeof(hdr), (char*)&hdr) <= 0) {
        kprint("[DL] cannot read ELF header\n");
        return 0;
    }
    if (*(uint32_t*)hdr.e_ident != ELF_MAGIC) {
        kprint("[DL] bad ELF magic\n");
        return 0;
    }
    if (hdr.e_machine != 3) { /* EM_386 */
        kprint("[DL] not i386\n");
        return 0;
    }

    uint32_t load_base = 0xFFFFFFFF;
    uint32_t load_end  = 0;

    for (int i = 0; i < hdr.e_phnum; i++) {
        Elf32_Phdr ph;
        read_vfs(file,
                 hdr.e_phoff + (uint32_t)i * hdr.e_phentsize,
                 sizeof(ph), (char*)&ph);
        if (ph.p_type == PT_LOAD && ph.p_memsz > 0) {
            if (ph.p_vaddr < load_base) load_base = ph.p_vaddr;
            uint32_t end = ph.p_vaddr + ph.p_memsz;
            if (end > load_end) load_end = end;
        }
    }

    if (load_base == 0xFFFFFFFF) {
        kprint("[DL] no PT_LOAD segments\n");
        return 0;
    }

    load_base &= ~(PAGE_SIZE - 1);

    for (int i = 0; i < hdr.e_phnum; i++) {
        Elf32_Phdr ph;
        read_vfs(file,
                 hdr.e_phoff + (uint32_t)i * hdr.e_phentsize,
                 sizeof(ph), (char*)&ph);

        if (ph.p_type != PT_LOAD || ph.p_memsz == 0) continue;

        uint32_t page_vaddr = ph.p_vaddr & ~(PAGE_SIZE - 1);
        uint32_t page_end   = (ph.p_vaddr + ph.p_memsz + PAGE_SIZE - 1)
                              & ~(PAGE_SIZE - 1);

        for (uint32_t va = page_vaddr; va < page_end; va += PAGE_SIZE) {
            void* phys = kalloc();
            if (!phys) {
                kprint("[DL] out of memory mapping segment\n");
                return 0;
            }
            if (proc_tracker_add(tracker, phys) < 0) {
                kfree_page(phys);
                kprint("[DL] tracker overflow\n");
                return 0;
            }

            uint8_t* p = (uint8_t*)phys;
            for (int k = 0; k < (int)PAGE_SIZE; k++) p[k] = 0;

            uint32_t page_file_start = va;
            uint32_t seg_file_end    = ph.p_vaddr + ph.p_filesz;

            if (page_file_start < seg_file_end) {
                uint32_t copy_start  = (page_file_start > ph.p_vaddr)
                                       ? page_file_start : ph.p_vaddr;
                uint32_t copy_end    = (page_file_start + PAGE_SIZE < seg_file_end)
                                       ? page_file_start + PAGE_SIZE : seg_file_end;
                uint32_t page_offset = copy_start - page_file_start;
                uint32_t file_offset = ph.p_offset + (copy_start - ph.p_vaddr);
                uint32_t copy_size   = copy_end - copy_start;

                read_vfs(file, file_offset, copy_size, (char*)phys + page_offset);
            }

            vmm_map(pd, va, (uint32_t)phys, PAGE_USER | PAGE_RW | PAGE_PRESENT);
        }

        if (ph.p_type == PT_DYNAMIC && out_dyn) {
            *out_dyn      = (Elf32_Dyn*)ph.p_vaddr;
            *out_dyn_size = ph.p_filesz;
        }
    }

    if (out_entry) *out_entry = hdr.e_entry;
    return load_base;
}

void dynlink_ctx_init(dyn_ctx_t* ctx, uint32_t* pd,
                      proc_page_tracker_t* tracker)
{
    ctx->pd      = pd;
    ctx->tracker = tracker;
    ctx->count   = 0;
    _so_strncpy(ctx->so_search_path, "/lib:/usr/lib", 256);
    for (int i = 0; i < SO_TABLE_MAX; i++) {
        ctx->table[i].ref_count = 0;
        ctx->table[i].name[0]   = '\0';
    }
}

static loaded_so_t* _find_loaded(dyn_ctx_t* ctx, const char* name) {
    for (int i = 0; i < ctx->count; i++) {
        if (_so_strcmp(ctx->table[i].name, name) == 0 &&
            ctx->table[i].ref_count > 0)
            return &ctx->table[i];
    }
    return 0;
}

static loaded_so_t* _alloc_so_slot(dyn_ctx_t* ctx) {
    if (ctx->count >= SO_TABLE_MAX) {
        kprint("[DL] so table full\n");
        return 0;
    }
    loaded_so_t* e = &ctx->table[ctx->count++];
    e->ref_count     = 0;
    e->load_base     = 0;
    e->load_size     = 0;
    e->symtab        = 0;
    e->symtab_count  = 0;
    e->strtab        = 0;
    e->init_addr     = 0;
    e->fini_addr     = 0;
    return e;
}

static void _fill_so_from_dynamic(loaded_so_t* so, Elf32_Dyn* dyn) {
    uint32_t strsz    = 0;
    uint32_t syment   = sizeof(Elf32_Sym);
    uint32_t symtabsz = 0; 

    uint32_t hash_addr = 0;

    for (Elf32_Dyn* d = dyn; d->d_tag != DT_NULL; d++) {
        switch (d->d_tag) {
        case DT_STRTAB:  so->strtab = (char*)d->d_un.d_ptr;           break;
        case DT_SYMTAB:  so->symtab = (Elf32_Sym*)d->d_un.d_ptr;      break;
        case DT_STRSZ:   strsz      = d->d_un.d_val;                  break;
        case DT_SYMENT:  syment     = d->d_un.d_val;                  break;
        case DT_HASH:    hash_addr  = d->d_un.d_ptr;                  break;
        case DT_INIT:    so->init_addr = d->d_un.d_ptr;               break;
        case DT_FINI:    so->fini_addr = d->d_un.d_ptr;               break;
        default: break;
        }
    }

    if (hash_addr && so->symtab) {
        uint32_t* ht       = (uint32_t*)hash_addr;
        so->symtab_count = ht[1];
    }

    (void)strsz; 
    (void)syment;
    (void)symtabsz;
}

loaded_so_t* dynlink_load_so(dyn_ctx_t* ctx, const char* name) {
    loaded_so_t* existing = _find_loaded(ctx, name);
    if (existing) {
        existing->ref_count++;
        return existing;
    }

    // Search path: "/lib:/usr/lib" etc. 
    char path[256];
    struct vfs_node* file = 0;

    const char* sp = ctx->so_search_path;
    while (*sp) {
        char dir[128];
        int di = 0;
        while (*sp && *sp != ':' && di < 127)
            dir[di++] = *sp++;
        dir[di] = '\0';
        if (*sp == ':') sp++;

        _so_path_join(path, 256, dir, name);
        file = finddir_vfs(vfs_root, path);
        if (file) break;
    }

    if (!file) {
        kprint("[DL] shared object not found: ");
        kprint((char*)name);
        kprint("\n");
        return 0;
    }

    loaded_so_t* so = _alloc_so_slot(ctx);
    if (!so) return 0;

    _so_strncpy(so->name, name, SO_NAME_MAX);

    Elf32_Dyn* dyn_seg  = 0;
    uint32_t   dyn_size = 0;
    uint32_t   entry    = 0;

    uint32_t base = _elf_map_file(file, ctx->pd, ctx->tracker,
                                   &entry, &dyn_seg, &dyn_size);
    if (!base) {
        kprint("[DL] failed to map: ");
        kprint((char*)name);
        kprint("\n");
        ctx->count--;  
        return 0;
    }

    so->load_base = base;
    so->ref_count = 1;

    if (dyn_seg)
        _fill_so_from_dynamic(so, dyn_seg);

    kprint("[DL] loaded ");
    kprint((char*)name);
    kprint("\n");

    if (dyn_seg)
        dynlink_process_dynamic(ctx, base, dyn_seg);

    return so;
}

uint32_t dynlink_resolve_symbol(dyn_ctx_t* ctx, const char* name) {
    for (int si = 0; si < ctx->count; si++) {
        loaded_so_t* so = &ctx->table[si];
        if (!so->symtab || !so->strtab || so->ref_count == 0) continue;

        for (uint32_t j = 1; j < so->symtab_count; j++) {
            Elf32_Sym* sym = &so->symtab[j];

            if (sym->st_shndx == SHN_UNDEF) continue;
            int bind = ELF32_ST_BIND(sym->st_info);
            if (bind != STB_GLOBAL && bind != STB_WEAK) continue;
            if (sym->st_value == 0) continue;

            const char* sym_name = so->strtab + sym->st_name;
            if (_so_strcmp(sym_name, name) == 0)
                return so->load_base + sym->st_value;
        }
    }
    return 0;
}

static void _apply_rel(dyn_ctx_t*   ctx,
                        uint32_t     load_base,
                        Elf32_Sym*   symtab,
                        char*        strtab,
                        Elf32_Rel*   rel)
{
    uint32_t  sym_idx  = ELF32_R_SYM(rel->r_info);
    uint8_t   rel_type = ELF32_R_TYPE(rel->r_info);
    uint32_t* target   = (uint32_t*)rel->r_offset; 

    uint32_t S = 0; 
    uint32_t A = 0; 
    uint32_t P = rel->r_offset;
    uint32_t B = load_base;

    if (sym_idx != 0 && symtab && strtab) {
        Elf32_Sym* sym = &symtab[sym_idx];
        if (sym->st_shndx == SHN_UNDEF) {
            const char* sname = strtab + sym->st_name;
            S = dynlink_resolve_symbol(ctx, sname);
            if (S == 0) {
                int bind = ELF32_ST_BIND(sym->st_info);
                if (bind != STB_WEAK) {
                    kprint("[DL] unresolved symbol: ");
                    kprint((char*)sname);
                    kprint("\n");
                }
            }
        } else {
            S = load_base + sym->st_value;
        }
    }

    switch (rel_type) {
    case R_386_NONE:
        break;

    case R_386_32:
        // word32   S + A 
        A = *target;
        *target = S + A;
        break;

    case R_386_PC32:
        // word32   S + A - P 
        A = *target;
        *target = S + A - P;
        break;

    case R_386_GLOB_DAT:
    case R_386_JMP_SLOT:
        // word32   S  (no addend for these) 
        *target = S;
        break;

    case R_386_RELATIVE:
        // word32   B + A 
        A = *target;
        *target = B + A;
        break;

    case R_386_COPY:
        if (S && sym_idx < (uint32_t)0xFFFFFFFF) {
            Elf32_Sym* sym = &symtab[sym_idx];
            if (sym->st_size > 0) {
                uint8_t* src = (uint8_t*)S;
                uint8_t* dst = (uint8_t*)target;
                for (uint32_t k = 0; k < sym->st_size; k++)
                    dst[k] = src[k];
            }
        }
        break;

    default:
        break;
    }
}

static void _apply_rela(dyn_ctx_t*  ctx,
                         uint32_t    load_base,
                         Elf32_Sym*  symtab,
                         char*       strtab,
                         Elf32_Rela* rela)
{
    uint32_t* target = (uint32_t*)rela->r_offset;
    *target = (uint32_t)rela->r_addend; 

    Elf32_Rel rel;
    rel.r_offset = rela->r_offset;
    rel.r_info   = rela->r_info;
    _apply_rel(ctx, load_base, symtab, strtab, &rel);
}

int dynlink_process_dynamic(dyn_ctx_t* ctx, uint32_t load_base,
                             Elf32_Dyn* dyn)
{
    Elf32_Sym* symtab      = 0;
    char*      strtab      = 0;
    uint32_t   symtab_count = 0;
    uint32_t   hash_addr   = 0;

    Elf32_Rel*  rel        = 0;
    uint32_t    rel_sz     = 0;
    uint32_t    rel_ent    = sizeof(Elf32_Rel);

    Elf32_Rela* rela       = 0;
    uint32_t    rela_sz    = 0;
    uint32_t    rela_ent   = sizeof(Elf32_Rela);

    Elf32_Rel*  jmprel     = 0;
    uint32_t    jmprel_sz  = 0;
    uint32_t    pltrel     = DT_REL; 

    for (Elf32_Dyn* d = dyn; d->d_tag != DT_NULL; d++) {
        switch (d->d_tag) {
        case DT_SYMTAB:   symtab    = (Elf32_Sym*)d->d_un.d_ptr; break;
        case DT_STRTAB:   strtab    = (char*)     d->d_un.d_ptr; break;
        case DT_HASH:     hash_addr = d->d_un.d_ptr;             break;
        case DT_REL:      rel       = (Elf32_Rel*)d->d_un.d_ptr; break;
        case DT_RELSZ:    rel_sz    = d->d_un.d_val;             break;
        case DT_RELENT:   rel_ent   = d->d_un.d_val;             break;
        case DT_RELA:     rela      = (Elf32_Rela*)d->d_un.d_ptr;break;
        case DT_RELASZ:   rela_sz   = d->d_un.d_val;             break;
        case DT_RELAENT:  rela_ent  = d->d_un.d_val;             break;
        case DT_JMPREL:   jmprel    = (Elf32_Rel*)d->d_un.d_ptr; break;
        case DT_PLTRELSZ: jmprel_sz = d->d_un.d_val;             break;
        case DT_PLTREL:   pltrel    = d->d_un.d_val;             break;
        default: break;
        }
    }

    if (hash_addr && symtab) {
        uint32_t* ht   = (uint32_t*)hash_addr;
        symtab_count   = ht[1]; 
    }

    (void)symtab_count; 

    for (Elf32_Dyn* d = dyn; d->d_tag != DT_NULL; d++) {
        if (d->d_tag == DT_NEEDED) {
            if (!strtab) {
                kprint("[DL] DT_NEEDED but no strtab\n");
                continue;
            }
            const char* soname = strtab + d->d_un.d_val;
            if (!_find_loaded(ctx, soname)) {
                dynlink_load_so(ctx, soname);
            }
        }
    }

    if (rel && rel_sz > 0) {
        uint32_t n = rel_sz / rel_ent;
        for (uint32_t i = 0; i < n; i++) {
            Elf32_Rel* r = (Elf32_Rel*)((uint8_t*)rel + i * rel_ent);
            _apply_rel(ctx, load_base, symtab, strtab, r);
        }
    }

    if (rela && rela_sz > 0) {
        uint32_t n = rela_sz / rela_ent;
        for (uint32_t i = 0; i < n; i++) {
            Elf32_Rela* r = (Elf32_Rela*)((uint8_t*)rela + i * rela_ent);
            _apply_rela(ctx, load_base, symtab, strtab, r);
        }
    }

    if (jmprel && jmprel_sz > 0) {
        if (pltrel == DT_RELA) {
            uint32_t n = jmprel_sz / sizeof(Elf32_Rela);
            for (uint32_t i = 0; i < n; i++) {
                Elf32_Rela* r = (Elf32_Rela*)((uint8_t*)jmprel + i * sizeof(Elf32_Rela));
                _apply_rela(ctx, load_base, symtab, strtab, r);
            }
        } else {
            uint32_t n = jmprel_sz / sizeof(Elf32_Rel);
            for (uint32_t i = 0; i < n; i++) {
                Elf32_Rel* r = (Elf32_Rel*)((uint8_t*)jmprel + i * sizeof(Elf32_Rel));
                _apply_rel(ctx, load_base, symtab, strtab, r);
            }
        }
    }

    return 0;
}


void dynlink_unload_all(dyn_ctx_t* ctx) {
    for (int i = 0; i < ctx->count; i++) {
        loaded_so_t* so = &ctx->table[i];
        if (so->ref_count <= 0) continue;
        so->ref_count--;
        if (so->ref_count == 0) {
            kprint("[DL] unloaded ");
            kprint(so->name);
            kprint("\n");
        }
    }
    ctx->count = 0;
}