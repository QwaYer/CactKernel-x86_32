#include "sym.h"
#include "elf.h"
#include "task.h"
#include "validate.h"

const char* sym_resolve_addr(uint32_t addr, uint32_t* offset) {
    if (offset) *offset = 0;

    if (!current_task || current_task->is_kernel || !current_task->proc)
        return 0;

    // Only a user address can be named from the process's own ELF.  A kernel
    // address (a fault caught in ring 0, a syscall return address) has nothing
    // to do with that symbol table; matching one here used to print nonsense
    // like `_GLOBAL_OFFSET_TABLE_+0xf0103166`, because the lookup kept the last
    // symbol at or below the wrapped offset.
    if (addr < USER_SPACE_START || addr >= KERNEL_BASE)
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
                // When the symbol carries a size, the address has to land
                // inside it — otherwise whatever symbol happens to be last
                // before `rel` swallows every address past the binary.
                uint32_t sym_size = symtab[j].st_size;
                if (sym_size > 0 && (rel - sym_val) >= sym_size)
                    continue;
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
