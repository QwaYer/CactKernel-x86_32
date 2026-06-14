#include "dynlink.h"
#include "vfs.h"
#include "memory.h"
#include "klib.h"
#include "kernel.h"
#include "page_fault.h"

static int _so_path_join(char* dst, int dst_sz,
                          const char* dir, const char* name)
{
    int dl = strlen(dir);
    int nl = strlen(name);
    if (dl + nl + 2 > dst_sz) return -1;
    strncpy(dst, dir, dst_sz);
    dst[dl]     = '/';
    dst[dl + 1] = '\0';
    strncpy(dst + dl + 1, name, dst_sz - dl - 1);
    return 0;
}

/* Returns 0 on success; *out_load_base is valid (may be 0 for ET_DYN at vaddr 0).
 * Returns -1 on failure. */
static int _elf_map_file(struct vfs_node*     file,
                          uint32_t*            pd,
                          proc_page_tracker_t* tracker,
                          uint32_t*            out_load_base,
                          uint32_t*            out_entry,
                          Elf32_Dyn**          out_dyn,
                          uint32_t*            out_dyn_size)
{
    Elf32_Ehdr hdr;
    if (read_vfs(file, 0, sizeof(hdr), (char*)&hdr) <= 0) {
        kprint("[DL] cannot read ELF header\n");
        return -1;
    }
    if (*(uint32_t*)hdr.e_ident != ELF_MAGIC) {
        kprint("[DL] bad ELF magic\n");
        return -1;
    }
    if (hdr.e_machine != 3) { // EM_386 
        kprint("[DL] not i386\n");
        return -1;
    }

    uint32_t load_base = 0xFFFFFFFF;
    uint32_t load_end  = 0;

    for (int i = 0; i < hdr.e_phnum; i++) {
        Elf32_Phdr ph;
        if (read_vfs(file,
                     hdr.e_phoff + (uint32_t)i * hdr.e_phentsize,
                     sizeof(ph), (char*)&ph) <= 0) {
            kprint("[DL] failed to read program header\n");
            return -1;
        }
        if (ph.p_type == PT_LOAD && ph.p_memsz > 0) {
            if (ph.p_vaddr < load_base) load_base = ph.p_vaddr;
            uint32_t end = ph.p_vaddr + ph.p_memsz;
            if (end > load_end) load_end = end;
        }
    }

    if (load_base == 0xFFFFFFFF) {
        kprint("[DL] no PT_LOAD segments\n");
        return -1;
    }

    load_base &= ~(PAGE_SIZE - 1);

    for (int i = 0; i < hdr.e_phnum; i++) {
        Elf32_Phdr ph;
        if (read_vfs(file,
                     hdr.e_phoff + (uint32_t)i * hdr.e_phentsize,
                     sizeof(ph), (char*)&ph) <= 0) {
            kprint("[DL] failed to read program header\n");
            return -1;
        }

        if (ph.p_type != PT_LOAD || ph.p_memsz == 0) continue;

        uint32_t page_vaddr = ph.p_vaddr & ~(PAGE_SIZE - 1);
        uint32_t page_end   = (ph.p_vaddr + ph.p_memsz + PAGE_SIZE - 1)
                              & ~(PAGE_SIZE - 1);

        for (uint32_t va = page_vaddr; va < page_end; va += PAGE_SIZE) {
            void* phys = kalloc();
            if (!phys) {
                kprint("[DL] out of memory mapping segment\n");
                return -1;
            }
            if (proc_tracker_add(tracker, phys) < 0) {
                kfree_page(phys);
                kprint("[DL] tracker overflow\n");
                return -1;
            }

            memset(phys, 0, PAGE_SIZE);

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

                read_vfs(file, file_offset, copy_size,
                         (char*)phys + page_offset);
            }

            vmm_map(pd, va, (uint32_t)phys, PAGE_USER | PAGE_RW | PAGE_PRESENT);
        }
    }

    if (out_dyn) {
        for (int i = 0; i < hdr.e_phnum; i++) {
            Elf32_Phdr ph;
            if (read_vfs(file,
                         hdr.e_phoff + (uint32_t)i * hdr.e_phentsize,
                         sizeof(ph), (char*)&ph) <= 0) {
                kprint("[DL] failed to read program header\n");
                return -1;
            }
            if (ph.p_type == PT_DYNAMIC) {
                /* CactOS-policy: ET_DYN с фиксированной базой (см. libc.ld)
                 * + ET_EXEC PIE одинаково маппятся ровно на свои p_vaddr.
                 * Значит bias = 0, и адреса в .dynamic, .symtab, .rel/.rel.plt
                 * уже абсолютные — никаких "load_base + ph.p_vaddr". */
                *out_dyn      = (Elf32_Dyn*)ph.p_vaddr;
                *out_dyn_size = ph.p_filesz;
                break;
            }
        }
    }

    /* Возвращаем bias (= 0 в нашей политике), а не "куда замаплено":
     * вся последующая адресная арифметика идёт через bias, и так упрощается
     * dynlink_resolve_symbol / _apply_rel — им не нужно знать e_type. */
    if (out_load_base) *out_load_base = 0;
    if (out_entry) *out_entry = hdr.e_entry;
    return 0;
}


void dynlink_ctx_init(dyn_ctx_t* ctx, uint32_t* pd,
                      proc_page_tracker_t* tracker)
{
    ctx->pd      = pd;
    ctx->tracker = tracker;
    ctx->count   = 0;
    strncpy(ctx->so_search_path, "/lib:/usr/lib", 256);
    for (int i = 0; i < SO_TABLE_MAX; i++) {
        ctx->table[i].ref_count = 0;
        ctx->table[i].name[0]   = '\0';
    }
}

static loaded_so_t* _find_loaded(dyn_ctx_t* ctx, const char* name) {
    for (int i = 0; i < ctx->count; i++) {
        if (strcmp(ctx->table[i].name, (char*)name) == 0 &&
            ctx->table[i].ref_count > 0)
            return &ctx->table[i];
    }
    return 0;
}

static loaded_so_t* _find_loaded_by_symtab(dyn_ctx_t* ctx, Elf32_Sym* sym) {
    if (!sym) return 0;
    for (int i = 0; i < ctx->count; i++) {
        if (ctx->table[i].ref_count > 0 && ctx->table[i].symtab == sym)
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
    e->ref_count    = 0;
    e->load_base    = 0;
    e->load_size    = 0;
    e->symtab       = 0;
    e->symtab_count = 0;
    e->strtab       = 0;
    e->init_addr    = 0;
    e->fini_addr    = 0;
    return e;
}

static uint32_t _symcount_from_gnu_hash(uint32_t gnu_hash_addr) {
    if (!gnu_hash_addr) return 0;

    uint32_t* gh = (uint32_t*)gnu_hash_addr;
    uint32_t nbuckets   = gh[0];
    uint32_t symoffset  = gh[1];
    uint32_t bloom_size = gh[2];

    if (nbuckets == 0) return 0;

    uint32_t* buckets = gh + 4 + bloom_size;
    uint32_t* chains  = buckets + nbuckets;

    uint32_t max_bucket_sym = 0;
    for (uint32_t i = 0; i < nbuckets; i++) {
        if (buckets[i] > max_bucket_sym)
            max_bucket_sym = buckets[i];
    }

    if (max_bucket_sym < symoffset)
        return symoffset;

    uint32_t chain_idx = max_bucket_sym - symoffset;
    for (uint32_t guard = 0; guard < 8192; guard++, chain_idx++) {
        uint32_t v = chains[chain_idx];
        if (v & 1u) {
            return symoffset + chain_idx + 1;
        }
    }

    // Conservative fallback when chain walk did not terminate as expected.
    return symoffset + 8192;
}

static void _fill_so_from_dynamic(loaded_so_t* so, Elf32_Dyn* dyn) {
    uint32_t hash_addr = 0;
    uint32_t gnu_hash_addr = 0;

    for (Elf32_Dyn* d = dyn; d->d_tag != DT_NULL; d++) {
        switch (d->d_tag) {
        case DT_STRTAB: so->strtab    = (char*)      d->d_un.d_ptr; break;
        case DT_SYMTAB: so->symtab    = (Elf32_Sym*) d->d_un.d_ptr; break;
        case DT_HASH:   hash_addr     = d->d_un.d_ptr;              break;
        case DT_GNU_HASH: gnu_hash_addr = d->d_un.d_ptr;            break;
        case DT_INIT:   so->init_addr = d->d_un.d_ptr;              break;
        case DT_FINI:   so->fini_addr = d->d_un.d_ptr;              break;
        default: break;
        }
    }

    if (hash_addr && so->symtab) {
        uint32_t* ht = (uint32_t*)hash_addr;
        so->symtab_count = ht[1];
    }

    if (so->symtab_count == 0 && gnu_hash_addr && so->symtab) {
        so->symtab_count = _symcount_from_gnu_hash(gnu_hash_addr);
    }

    if (so->symtab_count == 0 && so->symtab) {
        // Avoid resolving through an empty table when hash metadata is missing.
        so->symtab_count = 4096;
    }
}


loaded_so_t* dynlink_load_so(dyn_ctx_t* ctx, const char* name) {
    loaded_so_t* existing = _find_loaded(ctx, name);
    if (existing) {
        existing->ref_count++;
        return existing;
    }

    char             path[256];
    struct vfs_node* file = 0;

    const char* sp = ctx->so_search_path;
    while (*sp) {
        char dir[128];
        int  di = 0;
        while (*sp && *sp != ':' && di < 127)
            dir[di++] = *sp++;
        dir[di] = '\0';
        if (*sp == ':') sp++;

        if (_so_path_join(path, 256, dir, name) < 0) continue;
        file = vfs_walk_path(vfs_root, path);
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

    strncpy(so->name, name, SO_NAME_MAX);

    Elf32_Dyn* dyn_seg  = 0;
    uint32_t   dyn_size = 0;
    uint32_t   entry    = 0;

    uint32_t bias = 0;
    if (_elf_map_file(file, ctx->pd, ctx->tracker, &bias,
                      &entry, &dyn_seg, &dyn_size) != 0) {
        kprint("[DL] failed to map: ");
        kprint((char*)name);
        kprint("\n");
        ctx->count--;
        return 0;
    }

    /* В нашей политике bias всегда 0 — мы маппим so точно на VA, заданные в
     * линкер-скрипте. Поэтому symbol resolution `S = load_base + st_value`
     * с load_base=0 даёт корректный абсолютный адрес функции в libc.so. */
    so->load_base = bias;
    so->ref_count = 1;
    if (dyn_seg)
        _fill_so_from_dynamic(so, dyn_seg);

    if (dyn_seg)
        dynlink_process_dynamic(ctx, bias, bias, dyn_seg);

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
            if (strcmp((char*)sym_name, (char*)name) == 0)
                return so->load_base + sym->st_value;
        }
    }
    return 0;
}


static void _apply_rel(dyn_ctx_t*  ctx,
                        uint32_t    image_start,
                        uint32_t    sym_bias,
                        Elf32_Sym*  symtab,
                        char*       strtab,
                        Elf32_Rel*  rel)
{
    uint32_t  sym_idx  = ELF32_R_SYM(rel->r_info);
    uint8_t   rel_type = ELF32_R_TYPE(rel->r_info);
    uint32_t* target   = (uint32_t*)rel->r_offset;

    if (!vmm_is_user_address(rel->r_offset) ||
        !vmm_get_phys(ctx->pd, rel->r_offset)) {
        kprint("[DL] _apply_rel: invalid r_offset 0x"); kprint_hex(rel->r_offset); kprint("\n");
        return;
    }

    uint32_t S = 0;
    uint32_t A = 0;
    uint32_t P = rel->r_offset;
    uint32_t B = image_start;

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
            S = sym_bias + sym->st_value;
        }
    }

    switch (rel_type) {
    case R_386_NONE:
        break;

    case R_386_32:
        // word32  S + A  
        A = *target;
        *target = S + A;
        break;

    case R_386_PC32:
        // word32  S + A - P 
        A = *target;
        *target = S + A - P;
        break;

    case R_386_GLOB_DAT:
    case R_386_JMP_SLOT:
        // word32  S 
        *target = S;
        break;

    case R_386_RELATIVE:
        // word32  B + A 
        A = *target;
        *target = B + A;
        break;

    case R_386_COPY:
        if (S && symtab) {
            Elf32_Sym* sym = &symtab[sym_idx];
            if (sym->st_size > 0 && sym->st_size <= 65536) {
                if (!vmm_is_user_address(S) ||
                    !vmm_get_phys(ctx->pd, S)) {
                    kprint("[DL] R_386_COPY: invalid source 0x"); kprint_hex(S); kprint("\n");
                    break;
                }
                memcpy(target, (void*)S, sym->st_size);
            }
        }
        break;

    default:
        break;
    }
}

static void _apply_rela(dyn_ctx_t*  ctx,
                         uint32_t    image_start,
                         uint32_t    sym_bias,
                         Elf32_Sym*  symtab,
                         char*       strtab,
                         Elf32_Rela* rela)
{
    uint32_t  sym_idx  = ELF32_R_SYM(rela->r_info);
    uint8_t   rel_type = ELF32_R_TYPE(rela->r_info);
    uint32_t* target   = (uint32_t*)rela->r_offset;

    if (!vmm_is_user_address(rela->r_offset) ||
        !vmm_get_phys(ctx->pd, rela->r_offset)) {
        kprint("[DL] _apply_rela: invalid r_offset 0x"); kprint_hex(rela->r_offset); kprint("\n");
        return;
    }

    uint32_t S = 0;
    uint32_t A = (uint32_t)rela->r_addend; 
    uint32_t P = rela->r_offset;
    uint32_t B = image_start;

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
            S = sym_bias + sym->st_value;
        }
    }

    switch (rel_type) {
    case R_386_NONE:
        break;
    case R_386_32:
        *target = S + A;
        break;
    case R_386_PC32:
        *target = S + A - P;
        break;
    case R_386_GLOB_DAT:
    case R_386_JMP_SLOT:
        *target = S;
        break;
    case R_386_RELATIVE:
        *target = B + A;
        break;
    case R_386_COPY:
        if (S && symtab) {
            Elf32_Sym* sym = &symtab[sym_idx];
            if (sym->st_size > 0 && sym->st_size <= 65536) {
                if (!vmm_is_user_address(S) ||
                    !vmm_get_phys(ctx->pd, S)) {
                    kprint("[DL] R_386_COPY: invalid source 0x"); kprint_hex(S); kprint("\n");
                    break;
                }
                memcpy(target, (void*)S, sym->st_size);
            }
        }
        break;
    default:
        break;
    }
}


int dynlink_process_dynamic(dyn_ctx_t* ctx, uint32_t image_start, uint32_t sym_bias,
                             Elf32_Dyn* dyn)
{
    Elf32_Sym* symtab       = 0;
    char*      strtab       = 0;
    uint32_t   symtab_count = 0;
    uint32_t   hash_addr    = 0;
    uint32_t   gnu_hash_addr = 0;

    Elf32_Rel*  rel      = 0;
    uint32_t    rel_sz   = 0;
    uint32_t    rel_ent  = sizeof(Elf32_Rel);

    Elf32_Rela* rela     = 0;
    uint32_t    rela_sz  = 0;
    uint32_t    rela_ent = sizeof(Elf32_Rela);

    Elf32_Rel*  jmprel   = 0;
    uint32_t    jmprel_sz = 0;
    uint32_t    pltrel   = DT_REL;

    for (Elf32_Dyn* d = dyn; d->d_tag != DT_NULL; d++) {
        switch (d->d_tag) {
        case DT_SYMTAB:   symtab    = (Elf32_Sym*) d->d_un.d_ptr; break;
        case DT_STRTAB:   strtab    = (char*)      d->d_un.d_ptr; break;
        case DT_HASH:     hash_addr = d->d_un.d_ptr;              break;
        case DT_GNU_HASH: gnu_hash_addr = d->d_un.d_ptr;          break;
        case DT_REL:      rel       = (Elf32_Rel*) d->d_un.d_ptr; break;
        case DT_RELSZ:    rel_sz    = d->d_un.d_val;              break;
        case DT_RELENT:   rel_ent   = d->d_un.d_val;              break;
        case DT_RELA:     rela      = (Elf32_Rela*)d->d_un.d_ptr; break;
        case DT_RELASZ:   rela_sz   = d->d_un.d_val;              break;
        case DT_RELAENT:  rela_ent  = d->d_un.d_val;              break;
        case DT_JMPREL:   jmprel    = (Elf32_Rel*) d->d_un.d_ptr; break;
        case DT_PLTRELSZ: jmprel_sz = d->d_un.d_val;              break;
        case DT_PLTREL:   pltrel    = d->d_un.d_val;              break;
        default: break;
        }
    }

    if (hash_addr && symtab) {
        uint32_t* ht = (uint32_t*)hash_addr;
        symtab_count = ht[1];
    }
    if (symtab_count == 0 && gnu_hash_addr && symtab) {
        symtab_count = _symcount_from_gnu_hash(gnu_hash_addr);
    }
    if (symtab_count == 0 && symtab) {
        symtab_count = 4096;
    }
    /* Make current object visible for cross-object symbol resolution
     * (e.g. shared libs resolving symbols exported by main binary).
     * Идентифицируем уже-зарегистрированные объекты по адресу .symtab —
     * он уникален для каждого so и не зависит от bias-политики. */
    loaded_so_t* cur = (symtab && strtab) ? _find_loaded_by_symtab(ctx, symtab) : 0;
    if (symtab && strtab && !cur) {
        cur = _alloc_so_slot(ctx);
        if (cur)
            strncpy(cur->name, "<main>", SO_NAME_MAX);
    }
    if (cur) {
        cur->load_base = sym_bias;
        cur->symtab = symtab;
        cur->strtab = strtab;
        cur->symtab_count = symtab_count;
        cur->ref_count = 1;
    }

    for (Elf32_Dyn* d = dyn; d->d_tag != DT_NULL; d++) {
        if (d->d_tag == DT_NEEDED) {
            if (!strtab) {
                kprint("[DL] DT_NEEDED but no strtab\n");
                continue;
            }
            const char* soname = strtab + d->d_un.d_val;
            if (!_find_loaded(ctx, soname))
                dynlink_load_so(ctx, soname);
        }
    }

    if (rel && rel_sz > 0) {
        uint32_t n = rel_sz / rel_ent;
        for (uint32_t i = 0; i < n; i++) {
            Elf32_Rel* r = (Elf32_Rel*)((uint8_t*)rel + i * rel_ent);
            _apply_rel(ctx, image_start, sym_bias, symtab, strtab, r);
        }
    }

    if (rela && rela_sz > 0) {
        uint32_t n = rela_sz / rela_ent;
        for (uint32_t i = 0; i < n; i++) {
            Elf32_Rela* r = (Elf32_Rela*)((uint8_t*)rela + i * rela_ent);
            _apply_rela(ctx, image_start, sym_bias, symtab, strtab, r);
        }
    }

    if (jmprel && jmprel_sz > 0) {
        if (pltrel == DT_RELA) {
            uint32_t n = jmprel_sz / sizeof(Elf32_Rela);
            for (uint32_t i = 0; i < n; i++) {
                Elf32_Rela* r = (Elf32_Rela*)((uint8_t*)jmprel
                                               + i * sizeof(Elf32_Rela));
                _apply_rela(ctx, image_start, sym_bias, symtab, strtab, r);
            }
        } else {
            uint32_t n = jmprel_sz / sizeof(Elf32_Rel);
            for (uint32_t i = 0; i < n; i++) {
                Elf32_Rel* r = (Elf32_Rel*)((uint8_t*)jmprel
                                             + i * sizeof(Elf32_Rel));
                _apply_rel(ctx, image_start, sym_bias, symtab, strtab, r);
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
    }
    ctx->count = 0;
}

dyn_ctx_t* dynlink_ctx_create(uint32_t* pd, proc_page_tracker_t* tracker) {
    dyn_ctx_t* ctx = (dyn_ctx_t*)kmalloc(sizeof(dyn_ctx_t));
    if (!ctx) return 0;
    dynlink_ctx_init(ctx, pd, tracker);
    return ctx;
}

void dynlink_ctx_destroy(dyn_ctx_t* ctx) {
    if (!ctx) return;
    dynlink_unload_all(ctx);
    kfree_heap(ctx);
}