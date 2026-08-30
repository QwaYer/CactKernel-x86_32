#include "dynlink.h"
#include "dynlink_internal.h"
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

loaded_so_t* _find_loaded(dyn_ctx_t* ctx, const char* name) {
    for (int i = 0; i < ctx->count; i++) {
        if (strcmp(ctx->table[i].name, (char*)name) == 0 &&
            ctx->table[i].ref_count > 0)
            return &ctx->table[i];
    }
    return 0;
}

loaded_so_t* _find_loaded_by_symtab(dyn_ctx_t* ctx, Elf32_Sym* sym) {
    if (!sym) return 0;
    for (int i = 0; i < ctx->count; i++) {
        if (ctx->table[i].ref_count > 0 && ctx->table[i].symtab == sym)
            return &ctx->table[i];
    }
    return 0;
}

loaded_so_t* _alloc_so_slot(dyn_ctx_t* ctx) {
    if (ctx->count >= SO_TABLE_MAX) {
        printk("[DL] so table full\n");
        return 0;
    }
    loaded_so_t* e = &ctx->table[ctx->count++];
    e->ref_count    = 0;
    e->load_base    = 0;
    e->load_size    = 0;
    e->symtab       = 0;
    e->symtab_count = 0;
    e->strtab       = 0;
    e->strtab_size  = 0;
    e->init_addr    = 0;
    e->fini_addr    = 0;
    return e;
}

uint32_t _symcount_from_gnu_hash(uint32_t gnu_hash_addr) {
    if (!gnu_hash_addr) return 0;

    uint32_t* gh = (uint32_t*)gnu_hash_addr;
    uint32_t nbuckets   = gh[0];
    uint32_t symoffset  = gh[1];
    uint32_t bloom_size = gh[2];

    if (nbuckets == 0) return 0;

    if (bloom_size > 1024) bloom_size = 1024;

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

static void _fill_so_from_dynamic(loaded_so_t* so, Elf32_Dyn* dyn, uint32_t dyn_size) {
    uint32_t hash_addr = 0;
    uint32_t gnu_hash_addr = 0;
    uint32_t max_entries = dyn_size / sizeof(Elf32_Dyn);

    for (uint32_t i = 0; i < max_entries; i++) {
        Elf32_Dyn* d = &dyn[i];
        if (d->d_tag == DT_NULL) break;
        switch (d->d_tag) {
        case DT_STRTAB: so->strtab    = (char*)      d->d_un.d_ptr; break;
        case DT_STRSZ:  so->strtab_size = d->d_un.d_val;            break;
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
        printk("[DL] shared object not found: ");
        printk((char*)name);
        printk("\n");
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
        printk("[DL] failed to map: ");
        printk((char*)name);
        printk("\n");
        ctx->count--;
        return 0;
    }

    /* В нашей политике bias всегда 0 — мы маппим so точно на VA, заданные в
     * линкер-скрипте. Поэтому symbol resolution `S = load_base + st_value`
     * с load_base=0 даёт корректный абсолютный адрес функции в libc.so. */
    so->load_base = bias;
    so->ref_count = 1;
    if (dyn_seg)
        _fill_so_from_dynamic(so, dyn_seg, dyn_size);

    if (dyn_seg)
        dynlink_process_dynamic(ctx, bias, bias, dyn_seg, dyn_size);

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
            if (sym->st_name >= so->strtab_size) continue;

            const char* sym_name = so->strtab + sym->st_name;
            if (strcmp((char*)sym_name, (char*)name) == 0)
                return so->load_base + sym->st_value;
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
    if (ctx->tracker)
        proc_free_pages(ctx->tracker);
    kfree(ctx);
}
