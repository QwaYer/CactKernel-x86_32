#include "kernel.h"
#include "multiboot2.h"
#include "vfs.h"
#include "pci_driver.h"
#include "pcidev.h"
#include "task.h"
#include "fb.h"
#include "version.h"

/* Bootstrap thread — runs AFTER scheduler is live, so it can legitimately
 * sleep on semaphores when waiting for IRQ-driven NVMe/AHCI completions
 * during ext4 mount. Must NOT be executed from the boot context (which
 * has been claimed by the idle task and cannot be parked on a semaphore
 * because there is no other task to switch to). */
static multiboot_info_t* bootstrap_mbi = 0;
static void kernel_bootstrap_main(void) {
    extern void mntfs_init(void);
    extern void procfs_set_meminfo(uint32_t);

    pcidev_probe_all();

    // Module loading (PCI kmods, filesystem driver) is a userspace concern —
    // nothing is auto-loaded here. Without a filesystem module mntfs falls
    // back to a virtual nodisk root and the kernel still boots to /bin/init.
    mntfs_init();

    if (bootstrap_mbi) {
        uint32_t total_kb;
        if (bootstrap_mbi->flags & (1u << 6)) {
            total_kb = (uint32_t)(bootstrap_mbi->mem_total_bytes / 1024ull);
        } else {
            total_kb = bootstrap_mbi->mem_lower + 1024 + bootstrap_mbi->mem_upper;
        }
        procfs_set_meminfo(total_kb);
    }

    printk("\n");
    printk_color("Cact Kernel ", COLOR_LIGHT_BROWN);
    printk_color((char*)kernel_version, COLOR_LIGHT_BROWN);
    printk_color("\n", COLOR_LIGHT_BROWN);
    printk_color("--------------------------\n", COLOR_DARK_GREY);
    printk("[VER] commit="); printk((char*)kernel_commit_hash);
    printk("  built=");      printk((char*)kernel_build_time);
    printk("\n");

    printk_color("Kernel is ready. Launching init...\n", COLOR_LIGHT_GREEN);

    struct task_struct* init = create_elf_task("bin/init");
    if (!init) {
        printk_color("  boot        : FAILED — /bin/init not found\n", COLOR_LIGHT_RED);
    }

    /* Bootstrap thread is done. Yield forever so scheduler keeps running. */
    while (1) {
        __asm__ __volatile__("hlt");
    }
}

void kernel_spawn_bootstrap(multiboot_info_t* mbi) {
    bootstrap_mbi = mbi;
    create_task((void(*)())kernel_bootstrap_main);
}
