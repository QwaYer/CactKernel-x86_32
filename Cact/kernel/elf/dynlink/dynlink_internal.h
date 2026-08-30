#ifndef DYNLINK_INTERNAL_H
#define DYNLINK_INTERNAL_H

#include "dynlink.h"
#include "vfs.h"

/* dynlink_elf.c — maps a shared object file into the process address space. */
int _elf_map_file(struct vfs_node*     file,
                  uint32_t*            pd,
                  proc_page_tracker_t* tracker,
                  uint32_t*            out_load_base,
                  uint32_t*            out_entry,
                  Elf32_Dyn**          out_dyn,
                  uint32_t*            out_dyn_size);

/* dynlink.c — GNU hash table sizing helper, shared with the relocation pass. */
uint32_t _symcount_from_gnu_hash(uint32_t gnu_hash_addr);

/* dynlink.c — SO table helpers, shared with the relocation pass. */
loaded_so_t* _find_loaded(dyn_ctx_t* ctx, const char* name);
loaded_so_t* _find_loaded_by_symtab(dyn_ctx_t* ctx, Elf32_Sym* sym);
loaded_so_t* _alloc_so_slot(dyn_ctx_t* ctx);

#endif
