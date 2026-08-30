#include "dynlink.h"
#include "dynlink_internal.h"
#include "vfs.h"
#include "memory.h"
#include "klib.h"
#include "kernel.h"
#include "page_fault.h"

/* Returns 0 on success; *out_load_base is valid (may be 0 for ET_DYN at vaddr 0).
 * Returns -1 on failure. */
int _elf_map_file(struct vfs_node*     file,
                  uint32_t*            pd,
                  proc_page_tracker_t* tracker,
                  uint32_t*            out_load_base,
                  uint32_t*            out_entry,
                  Elf32_Dyn**          out_dyn,
                  uint32_t*            out_dyn_size)
{
    Elf32_Ehdr hdr;
    if (read_vfs(file, 0, sizeof(hdr), (char*)&hdr) <= 0) {
        printk("[DL] cannot read ELF header\n");
        return -1;
    }
    if (*(uint32_t*)hdr.e_ident != ELF_MAGIC) {
        printk("[DL] bad ELF magic\n");
        return -1;
    }
    if (hdr.e_machine != 3) { // EM_386 
        printk("[DL] not i386\n");
        return -1;
    }
    if ((uint32_t)hdr.e_phnum > 65535u) {
        printk("[DL] too many program headers\n");
        return -1;
    }

    uint32_t load_base = 0xFFFFFFFF;
    uint32_t load_end  = 0;

    for (int i = 0; i < hdr.e_phnum; i++) {
        Elf32_Phdr ph;
        if (read_vfs(file,
                     hdr.e_phoff + (uint32_t)i * hdr.e_phentsize,
                     sizeof(ph), (char*)&ph) <= 0) {
            printk("[DL] failed to read program header\n");
            return -1;
        }
        if (ph.p_type == PT_LOAD && ph.p_memsz > 0) {
            if (ph.p_vaddr < load_base) load_base = ph.p_vaddr;
            uint32_t end = ph.p_vaddr + ph.p_memsz;
            if (end > load_end) load_end = end;
        }
    }

    if (load_base == 0xFFFFFFFF) {
        printk("[DL] no PT_LOAD segments\n");
        return -1;
    }

    load_base &= ~(PAGE_SIZE - 1);

    for (int i = 0; i < hdr.e_phnum; i++) {
        Elf32_Phdr ph;
        if (read_vfs(file,
                     hdr.e_phoff + (uint32_t)i * hdr.e_phentsize,
                     sizeof(ph), (char*)&ph) <= 0) {
            printk("[DL] failed to read program header\n");
            return -1;
        }

        if (ph.p_type != PT_LOAD || ph.p_memsz == 0) continue;

        uint32_t page_vaddr = ph.p_vaddr & ~(PAGE_SIZE - 1);
        uint32_t page_end   = (ph.p_vaddr + ph.p_memsz + PAGE_SIZE - 1)
                              & ~(PAGE_SIZE - 1);

        for (uint32_t va = page_vaddr; va < page_end; va += PAGE_SIZE) {
            void* phys = kalloc();
            if (!phys) {
                printk("[DL] out of memory mapping segment\n");
                proc_free_pages(tracker);
                return -1;
            }
            if (proc_tracker_add(tracker, phys) < 0) {
                free_page(phys);
                printk("[DL] tracker overflow\n");
                proc_free_pages(tracker);
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
                printk("[DL] failed to read program header\n");
                proc_free_pages(tracker);
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
