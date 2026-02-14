#include "elf.h"
#include "kernel.h"
#include "vfs.h"
#include "memory.h"
#include "libc.h"

void* load_elf(char* path, uint32_t* pd) {
    struct vfs_node* file = finddir_vfs(vfs_root, path);
    if (!file) {
        kprint("ELF: File not found\n");
        return 0;
    }

    Elf32_Ehdr header;
    if (read_vfs(file, 0, sizeof(Elf32_Ehdr), (char*)&header) <= 0) {
        kprint("ELF: Failed to read header\n");
        return 0;
    }

    if (*(uint32_t*)header.e_ident != ELF_MAGIC) {
        kprint("ELF: Invalid magic\n");
        return 0;
    }

    Elf32_Phdr phdr;
    for (int i = 0; i < header.e_phnum; i++) {
        read_vfs(file, header.e_phoff + i * header.e_phentsize, sizeof(Elf32_Phdr), (char*)&phdr);

        if (phdr.p_type == PT_LOAD) {
            /* Выделяем страницы и мапим их */
            uint32_t num_pages = (phdr.p_memsz + PAGE_SIZE - 1) / PAGE_SIZE;
            uint32_t vaddr = phdr.p_vaddr;

            for (uint32_t p = 0; p < num_pages; p++) {
                void* phys = kalloc();
                vmm_map(pd, vaddr + p * PAGE_SIZE, (uint32_t)phys, PAGE_USER | PAGE_RW);
            }

            /* Копируем данные. 
               Внимание: сейчас мы в адресном пространстве ядра. 
               Чтобы скопировать в новое пространство, нужно либо временно переключиться,
               либо мапить физические страницы ядра. 
               Для простоты пока переключимся (небезопасно, но для начала пойдет).
            */
            uint32_t* old_pd = (uint32_t*)get_current_pd();
            switch_paging(pd);
            
            /* Обнуляем память (bss) */
            memset((void*)phdr.p_vaddr, 0, phdr.p_memsz);
            /* Читаем из файла */
            read_vfs(file, phdr.p_offset, phdr.p_filesz, (char*)phdr.p_vaddr);

            switch_paging(old_pd);
        }
    }

    return (void*)header.e_entry;
}
