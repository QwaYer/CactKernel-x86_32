#include "sym.h"
#include "elf.h"
#include "task.h"

const char* sym_resolve_addr(uint32_t addr, uint32_t* offset) {
    if (offset) *offset = 0;

    if (!current_task || current_task->is_kernel || !current_task->proc)
        return 0;

    // Fall back to the cached main-binary symbol table (ET_EXEC/PIE)
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
