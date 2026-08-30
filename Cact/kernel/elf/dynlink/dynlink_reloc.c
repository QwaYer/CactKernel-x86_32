#include "dynlink.h"
#include "dynlink_internal.h"
#include "vfs.h"
#include "memory.h"
#include "klib.h"
#include "kernel.h"
#include "page_fault.h"

static void _apply_rel(dyn_ctx_t*  ctx,
                        uint32_t    image_start,
                        uint32_t    sym_bias,
                        Elf32_Sym*  symtab,
                        char*       strtab,
                        uint32_t    strtab_size,
                        Elf32_Rel*  rel)
{
    uint32_t  sym_idx  = ELF32_R_SYM(rel->r_info);
    uint8_t   rel_type = ELF32_R_TYPE(rel->r_info);
    uint32_t* target   = (uint32_t*)rel->r_offset;

    if (!vmm_is_user_address(rel->r_offset) ||
        !vmm_get_phys(ctx->pd, rel->r_offset)) {
        printk("[DL] _apply_rel: invalid r_offset 0x"); printk_hex(rel->r_offset); printk("\n");
        return;
    }

    uint32_t S = 0;
    uint32_t A = 0;
    uint32_t P = rel->r_offset;
    uint32_t B = image_start;

    if (sym_idx != 0 && symtab && strtab) {
        Elf32_Sym* sym = &symtab[sym_idx];
        if (sym->st_shndx == SHN_UNDEF) {
            if (sym->st_name >= strtab_size) {
                printk("[DL] _apply_rel: strtab offset out of bounds\n");
                return;
            }
            const char* sname = strtab + sym->st_name;
            S = dynlink_resolve_symbol(ctx, sname);
            if (S == 0) {
                int bind = ELF32_ST_BIND(sym->st_info);
                if (bind != STB_WEAK) {
                    printk("[DL] unresolved symbol: ");
                    printk((char*)sname);
                    printk("\n");
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
                    printk("[DL] R_386_COPY: invalid source 0x"); printk_hex(S); printk("\n");
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
                          uint32_t    strtab_size,
                          Elf32_Rela* rela)
{
    uint32_t  sym_idx  = ELF32_R_SYM(rela->r_info);
    uint8_t   rel_type = ELF32_R_TYPE(rela->r_info);
    uint32_t* target   = (uint32_t*)rela->r_offset;

    if (!vmm_is_user_address(rela->r_offset) ||
        !vmm_get_phys(ctx->pd, rela->r_offset)) {
        printk("[DL] _apply_rela: invalid r_offset 0x"); printk_hex(rela->r_offset); printk("\n");
        return;
    }

    uint32_t S = 0;
    uint32_t A = (uint32_t)rela->r_addend; 
    uint32_t P = rela->r_offset;
    uint32_t B = image_start;

    if (sym_idx != 0 && symtab && strtab) {
        Elf32_Sym* sym = &symtab[sym_idx];
        if (sym->st_shndx == SHN_UNDEF) {
            if (sym->st_name >= strtab_size) {
                printk("[DL] _apply_rela: strtab offset out of bounds\n");
                return;
            }
            const char* sname = strtab + sym->st_name;
            S = dynlink_resolve_symbol(ctx, sname);
            if (S == 0) {
                int bind = ELF32_ST_BIND(sym->st_info);
                if (bind != STB_WEAK) {
                    printk("[DL] unresolved symbol: ");
                    printk((char*)sname);
                    printk("\n");
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
                    printk("[DL] R_386_COPY: invalid source 0x"); printk_hex(S); printk("\n");
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
                             Elf32_Dyn* dyn, uint32_t dyn_size)
{
    Elf32_Sym* symtab       = 0;
    char*      strtab       = 0;
    uint32_t   strtab_size  = 0;
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

    uint32_t max_entries = dyn_size / sizeof(Elf32_Dyn);

    for (uint32_t i = 0; i < max_entries; i++) {
        Elf32_Dyn* d = &dyn[i];
        if (d->d_tag == DT_NULL) break;
        switch (d->d_tag) {
        case DT_SYMTAB:   symtab    = (Elf32_Sym*) d->d_un.d_ptr; break;
        case DT_STRTAB:   strtab    = (char*)      d->d_un.d_ptr; break;
        case DT_STRSZ:    strtab_size = d->d_un.d_val;            break;
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
        cur->strtab_size = strtab_size;
        cur->symtab_count = symtab_count;
        cur->ref_count = 1;
    }

    for (uint32_t i = 0; i < max_entries; i++) {
        Elf32_Dyn* d = &dyn[i];
        if (d->d_tag == DT_NULL) break;
        if (d->d_tag == DT_NEEDED) {
            if (!strtab) {
                printk("[DL] DT_NEEDED but no strtab\n");
                continue;
            }
            if (d->d_un.d_val >= strtab_size) {
                printk("[DL] DT_NEEDED: strtab offset out of bounds\n");
                continue;
            }
            const char* soname = strtab + d->d_un.d_val;
            if (!_find_loaded(ctx, soname))
                dynlink_load_so(ctx, soname);
        }
    }

    if (rel && rel_sz > 0 && rel_ent > 0) {
        if (rel_ent != sizeof(Elf32_Rel)) {
            printk("[DL] DT_RELENT != sizeof(Elf32_Rel)\n");
            return -1;
        }
        uint32_t n = rel_sz / rel_ent;
        for (uint32_t i = 0; i < n; i++) {
            Elf32_Rel* r = (Elf32_Rel*)((uint8_t*)rel + i * rel_ent);
            _apply_rel(ctx, image_start, sym_bias, symtab, strtab, strtab_size, r);
        }
    }

    if (rela && rela_sz > 0 && rela_ent > 0) {
        if (rela_ent != sizeof(Elf32_Rela)) {
            printk("[DL] DT_RELAENT != sizeof(Elf32_Rela)\n");
            return -1;
        }
        uint32_t n = rela_sz / rela_ent;
        for (uint32_t i = 0; i < n; i++) {
            Elf32_Rela* r = (Elf32_Rela*)((uint8_t*)rela + i * rela_ent);
            _apply_rela(ctx, image_start, sym_bias, symtab, strtab, strtab_size, r);
        }
    }

    if (jmprel && jmprel_sz > 0) {
        if (pltrel == DT_RELA) {
            uint32_t n = jmprel_sz / sizeof(Elf32_Rela);
            for (uint32_t i = 0; i < n; i++) {
                Elf32_Rela* r = (Elf32_Rela*)((uint8_t*)jmprel
                                               + i * sizeof(Elf32_Rela));
                _apply_rela(ctx, image_start, sym_bias, symtab, strtab, strtab_size, r);
            }
        } else {
            uint32_t n = jmprel_sz / sizeof(Elf32_Rel);
            for (uint32_t i = 0; i < n; i++) {
                Elf32_Rel* r = (Elf32_Rel*)((uint8_t*)jmprel
                                             + i * sizeof(Elf32_Rel));
                _apply_rel(ctx, image_start, sym_bias, symtab, strtab, strtab_size, r);
            }
        }
    }
    return 0;
}
