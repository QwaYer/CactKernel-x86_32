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

    if (header.e_machine != 3) { /* EM_386 */
        kprint("ELF: Not i386\n");
        return 0;
    }

    Elf32_Phdr phdr;
    for (int i = 0; i < header.e_phnum; i++) {
        read_vfs(file,
                 header.e_phoff + i * header.e_phentsize,
                 sizeof(Elf32_Phdr),
                 (char*)&phdr);

        if (phdr.p_type != PT_LOAD) continue;
        if (phdr.p_memsz == 0) continue;

        uint32_t num_pages = (phdr.p_memsz + PAGE_SIZE - 1) / PAGE_SIZE;

        for (uint32_t p = 0; p < num_pages; p++) {
            uint32_t vaddr = phdr.p_vaddr + p * PAGE_SIZE;

            /* Выделяем физическую страницу */
            void* phys = kalloc();
            if (!phys) {
                kprint("ELF: out of physical memory\n");
                return 0;
            }

            /* Маппим в новый PD для процесса */
            vmm_map(pd, vaddr, (uint32_t)phys, PAGE_USER | PAGE_RW | PAGE_PRESENT);

            /*
             * КЛЮЧЕВОЙ МОМЕНТ: не переключаем PD!
             * Физическая страница уже лежит в ядровом пространстве
             * (первые 128MB = identity map), поэтому пишем напрямую
             * по физическому адресу.
             */
            uint32_t page_offset = p * PAGE_SIZE;
            uint32_t seg_offset  = (page_offset < phdr.p_filesz) ? page_offset : phdr.p_filesz;
            uint32_t copy_size   = 0;

            if (page_offset < phdr.p_filesz) {
                copy_size = phdr.p_filesz - page_offset;
                if (copy_size > PAGE_SIZE) copy_size = PAGE_SIZE;
            }

            /* Сначала зануляем страницу (bss / выравнивание) */
            memset(phys, 0, PAGE_SIZE);

            /* Потом копируем данные из файла если есть */
            if (copy_size > 0) {
                read_vfs(file,
                         phdr.p_offset + page_offset,
                         copy_size,
                         (char*)phys);
            }
        }
    }

    /* Обязательно мапим ядро в новое адресное пространство, если vmm_create_address_space этого не сделал */
    /* Обычно vmm_create_address_space копирует kernel mapping из kernel_directory */

    return (void*)header.e_entry;
}