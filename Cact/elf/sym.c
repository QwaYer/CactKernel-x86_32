#include "sym.h"
#include "elf.h"
#include "dynlink.h"
#include "task.h"

const char* sym_resolve_addr(uint32_t addr, uint32_t* offset) {
    if (offset) *offset = 0;

    if (!current_task || current_task->is_kernel || !current_task->proc)
        return 0;

    // 1) Scan dynlink context (shared objects)
    dyn_ctx_t* ctx = current_task->proc->dyn_ctx;
    if (ctx) {
        for (int i = 0; i < ctx->count; i++) {
            loaded_so_t* so = &ctx->table[i];
            if (!so->symtab || !so->strtab || so->symtab_count == 0)
                continue;

            uint32_t base = so->load_base;
            uint32_t end = base + so->load_size;

            if (addr < base || addr >= end)
                continue;

            uint32_t rel = addr - base;
            int best = -1;
            uint32_t best_val = 0;

            for (int j = 0; j < so->symtab_count; j++) {
                uint32_t sym_val = so->symtab[j].st_value;
                uint8_t st_type = ELF32_ST_TYPE(so->symtab[j].st_info);
                if (sym_val > 0 && sym_val <= rel &&
                    (st_type == STT_FUNC || st_type == STT_NOTYPE || st_type == STT_OBJECT)) {
                    if (best < 0 || (rel - sym_val) < (rel - best_val)) {
                        best = j;
                        best_val = sym_val;
                    }
                }
            }

            if (best >= 0) {
                if (offset) *offset = rel - best_val;
                return &so->strtab[so->symtab[best].st_name];
            }

            return 0;
        }
    }

    // 2) Fall back to cached main-binary symbol table (ET_EXEC)
    Elf32_Sym* symtab = (Elf32_Sym*)current_task->proc->exec_symtab;
    char*      strtab = current_task->proc->exec_strtab;
    int        count  = current_task->proc->exec_symtab_count;
    uint32_t   base   = current_task->proc->exec_base;

    if (symtab && strtab && count > 0) {
        uint32_t rel = addr - base;
        int best = -1;
        uint32_t best_val = 0;

        for (int j = 0; j < count; j++) {
            uint32_t sym_val = symtab[j].st_value;
            uint8_t st_type = ELF32_ST_TYPE(symtab[j].st_info);
            if (sym_val > 0 && sym_val <= rel &&
                (st_type == STT_FUNC || st_type == STT_NOTYPE || st_type == STT_OBJECT)) {
                if (best < 0 || (rel - sym_val) < (rel - best_val)) {
                    best = j;
                    best_val = sym_val;
                }
            }
        }

        if (best >= 0) {
            if (offset) *offset = rel - best_val;
            return &strtab[symtab[best].st_name];
        }
    }

    return 0;
}
