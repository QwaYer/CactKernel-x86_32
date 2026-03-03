#include "elf.h"
#include "dynlink.h"
#include "kernel.h"
#include "vfs.h"
#include "memory.h"
#include "page_fault.h"
#include "proc_mm.h"
#include "libc.h"


void* load_elf(char* path, uint32_t* pd, proc_page_tracker_t* tracker)
{
    tracker->page_dir = pd;

    struct vfs_node* file = finddir_vfs(vfs_root, path);
    if (!file) {
        kprint("ELF: file not found\n");
        return 0;
    }

    Elf32_Ehdr hdr;
    if (read_vfs(file, 0, sizeof(Elf32_Ehdr), (char*)&hdr) <= 0) {
        kprint("ELF: cannot read header\n");
        return 0;
    }

    if (*(uint32_t*)hdr.e_ident != ELF_MAGIC) {
        kprint("ELF: bad magic\n");
        return 0;
    }
    if (hdr.e_machine != 3) { /* EM_386 */
        kprint("ELF: not i386\n");
        return 0;
    }

    for (int i = 0; i < hdr.e_phnum; i++) {
        Elf32_Phdr ph;
        if (read_vfs(file,
                     hdr.e_phoff + (uint32_t)i * hdr.e_phentsize,
                     sizeof(Elf32_Phdr),
                     (char*)&ph) <= 0) {
            kprint("ELF: cannot read phdr\n");
            proc_free_pages(tracker);
            return 0;
        }

        if (ph.p_type != PT_LOAD || ph.p_memsz == 0) continue;

        uint32_t file_pages = (ph.p_filesz + 0xFFF) >> 12;
        uint32_t mem_pages  = (ph.p_memsz  + 0xFFF) >> 12;

        for (uint32_t p = 0; p < file_pages; p++) {
            uint32_t vaddr = ph.p_vaddr + p * PAGE_SIZE;

            void* phys = kalloc();
            if (!phys) {
                kprint("ELF: out of memory (code/data)\n");
                proc_free_pages(tracker);
                return 0;
            }
            if (proc_tracker_add(tracker, phys) < 0) {
                kprint("ELF: tracker overflow\n");
                kfree_page(phys);
                proc_free_pages(tracker);
                return 0;
            }

            uint8_t* b = (uint8_t*)phys;
            for (int k = 0; k < (int)PAGE_SIZE; k++) b[k] = 0;

            uint32_t file_off    = p * PAGE_SIZE;
            uint32_t bytes_left  = ph.p_filesz - file_off;
            uint32_t copy_size   = (bytes_left > PAGE_SIZE) ? PAGE_SIZE : bytes_left;

            if (copy_size > 0) {
                if (read_vfs(file,
                             ph.p_offset + file_off,
                             copy_size,
                             (char*)phys) <= 0) {
                    kprint("ELF: read error\n");
                    proc_free_pages(tracker);
                    return 0;
                }
            }

            vmm_map(pd, vaddr, (uint32_t)phys,
                    PAGE_USER | PAGE_RW | PAGE_PRESENT);
        }

        if (mem_pages > file_pages) {
            uint32_t bss_start = ph.p_vaddr + file_pages * PAGE_SIZE;
            uint32_t bss_size  = (mem_pages - file_pages) * PAGE_SIZE;

            if (vmm_map_zero(pd, bss_start, bss_size, PAGE_USER) != 0) {
                kprint("ELF: vmm_map_zero failed (BSS)\n");
                proc_free_pages(tracker);
                return 0;
            }
        }
    }

    return (void*)hdr.e_entry;
}

void* load_elf_dynamic(char*                path,
                        uint32_t*            pd,
                        proc_page_tracker_t* tracker,
                        dyn_ctx_t*           ctx)
{
    tracker->page_dir = pd;

    struct vfs_node* file = finddir_vfs(vfs_root, path);
    if (!file) {
        kprint("ELF: file not found: ");
        kprint(path);
        kprint("\n");
        return 0;
    }

    Elf32_Ehdr hdr;
    if (read_vfs(file, 0, sizeof(Elf32_Ehdr), (char*)&hdr) <= 0) {
        kprint("ELF: cannot read header\n");
        return 0;
    }
    if (*(uint32_t*)hdr.e_ident != ELF_MAGIC) {
        kprint("ELF: bad magic\n");
        return 0;
    }
    if (hdr.e_machine != 3) { /* EM_386 */
        kprint("ELF: not i386\n");
        return 0;
    }

    Elf32_Dyn* dyn_vaddr = 0;   
    uint32_t   dyn_size  = 0;

    for (int i = 0; i < hdr.e_phnum; i++) {
        Elf32_Phdr ph;
        if (read_vfs(file,
                     hdr.e_phoff + (uint32_t)i * hdr.e_phentsize,
                     sizeof(Elf32_Phdr),
                     (char*)&ph) <= 0) {
            kprint("ELF: cannot read phdr\n");
            proc_free_pages(tracker);
            return 0;
        }

        if (ph.p_type == PT_LOAD && ph.p_memsz > 0) {
            uint32_t file_pages = (ph.p_filesz + 0xFFF) >> 12;
            uint32_t mem_pages  = (ph.p_memsz  + 0xFFF) >> 12;

            for (uint32_t p = 0; p < file_pages; p++) {
                uint32_t vaddr = ph.p_vaddr + p * PAGE_SIZE;

                void* phys = kalloc();
                if (!phys) {
                    kprint("ELF: out of memory\n");
                    proc_free_pages(tracker);
                    return 0;
                }
                if (proc_tracker_add(tracker, phys) < 0) {
                    kprint("ELF: tracker overflow\n");
                    kfree_page(phys);
                    proc_free_pages(tracker);
                    return 0;
                }

                uint8_t* b = (uint8_t*)phys;
                for (int k = 0; k < (int)PAGE_SIZE; k++) b[k] = 0;

                uint32_t file_off  = p * PAGE_SIZE;
                uint32_t remaining = ph.p_filesz - file_off;
                uint32_t copy_sz   = (remaining > PAGE_SIZE) ? PAGE_SIZE : remaining;

                if (copy_sz > 0) {
                    if (read_vfs(file,
                                 ph.p_offset + file_off,
                                 copy_sz,
                                 (char*)phys) <= 0) {
                        kprint("ELF: read error\n");
                        proc_free_pages(tracker);
                        return 0;
                    }
                }

                vmm_map(pd, vaddr, (uint32_t)phys,
                        PAGE_USER | PAGE_RW | PAGE_PRESENT);
            }

            if (mem_pages > file_pages) {
                uint32_t bss_start = ph.p_vaddr + file_pages * PAGE_SIZE;
                uint32_t bss_size  = (mem_pages - file_pages) * PAGE_SIZE;

                if (vmm_map_zero(pd, bss_start, bss_size, PAGE_USER) != 0) {
                    kprint("ELF: vmm_map_zero failed (BSS)\n");
                    proc_free_pages(tracker);
                    return 0;
                }
            }
        }

        if (ph.p_type == PT_DYNAMIC) {
            dyn_vaddr = (Elf32_Dyn*)ph.p_vaddr;
            dyn_size  = ph.p_filesz;
        }
    }

    if (dyn_vaddr) {
        dynlink_ctx_init(ctx, pd, tracker);

        uint32_t load_base = 0;

        int rc = dynlink_process_dynamic(ctx, load_base, dyn_vaddr);
        if (rc != 0) {
            kprint("ELF: dynamic linking failed\n");
            proc_free_pages(tracker);
            return 0;
        }

        kprint("ELF: dynamic linking complete\n");
    } else {
        kprint("ELF: no PT_DYNAMIC — treating as static\n");
    }

    return (void*)hdr.e_entry;
}