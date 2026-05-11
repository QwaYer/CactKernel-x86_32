#include "kernel.h"
#include "multiboot2.h"
#include "pci.h"
#include "pci_enum.h"
#include "pci_driver.h"
#include "ps_2_keyboard.h"
#include "keyboard.h"
#include "ps_2_mouse.h"
#include "memory.h"
#include "gdt.h"
#include "idt.h"
#include "vfs.h"
#include "devfs.h"
#include "klib.h"
#include "task.h"
#include "fb.h"
#include "swap.h"
#include "pagecache.h"
#include "blkdev.h"
#include "version.h"
#include "serial.h"

// Kernel page directory (defined in paging.c)
extern uint32_t page_directory[1024];

// Terminal window size for ioctl() calls
struct winsize terminal_winsize;
uint32_t       terminal_fg_pid = 0;

// Probe PS/2 controller via port 0x64, return 1 if absent
int probe_io_ports() {
    if (port_byte_in(0x64) == 0xFF) return 1;
    return 0;
}

// Read extended memory size from CMOS (regs 0x17/0x18), return 0 if valid
int detect_memory() {
    port_byte_out(0x70, 0x17);
    unsigned char low = port_byte_in(0x71);
    port_byte_out(0x70, 0x18);
    unsigned char high = port_byte_in(0x71);
    return ((high << 8) | low) > 0 ? 0 : 1;
}

// CPU exception handler — signals for user tasks, panic for kernel
void exception_handler(struct context_frame* regs) {
    char buf[32];

    // User mode exception → deliver POSIX signal
    if (current_task && !current_task->is_kernel) {
        uint32_t sig;
        switch (regs->int_no) {
        case 0:  // #DE Divide Error
        case 16: // #MF x87 FPU exception
            sig = SIGFPE;
            kprint_color("\n[KERNEL] User process: FPE (int=", COLOR_LIGHT_RED);
            break;
        case 13: // #GP General Protection Fault
            sig = SIGSEGV;
            kprint_color("\n[KERNEL] User process: SEGV (int=", COLOR_LIGHT_RED);
            break;
        default:
            sig = SIGKILL;
            kprint_color("\n[KERNEL] User process crashed (int=", COLOR_LIGHT_RED);
            break;
        }
        itoa((int)regs->int_no, buf); kprint_color(buf, COLOR_LIGHT_RED);
        kprint_color(") err=", COLOR_LIGHT_RED); hex_to_ascii(regs->err_code, buf); kprint_color(buf, COLOR_LIGHT_RED);
        kprint_color(" eip=", COLOR_LIGHT_RED); hex_to_ascii(regs->eip, buf); kprint_color(buf, COLOR_LIGHT_RED);
        kprint("\n");
        task_signal(current_task->pid, sig);
        schedule();
        return;
    }

    // Kernel mode exception → fatal panic
    kprint_color("\n=== KERNEL PANIC ===\n", COLOR_LIGHT_RED);
    kprint("Exception: "); itoa((int)regs->int_no, buf); kprint(buf);
    kprint(" Error code: "); hex_to_ascii(regs->err_code, buf); kprint(buf);
    kprint("\nEIP: "); hex_to_ascii(regs->eip, buf); kprint(buf);
    kprint(" CS: "); hex_to_ascii(regs->cs, buf); kprint(buf);
    kprint("\nEAX: "); hex_to_ascii(regs->eax, buf); kprint(buf);
    kprint(" EBX: "); hex_to_ascii(regs->ebx, buf); kprint(buf);
    kprint(" ECX: "); hex_to_ascii(regs->ecx, buf); kprint(buf);
    kprint(" EDX: "); hex_to_ascii(regs->edx, buf); kprint(buf);
    kprint("\nESP: "); hex_to_ascii(regs->esp_dummy, buf); kprint(buf);
    kprint(" EBP: "); hex_to_ascii(regs->ebp, buf); kprint(buf);
    kprint("\n");
    kprint_color("System halted.", COLOR_LIGHT_RED);
    while(1);
}

// Initialize PIT (8253) timer at given frequency (Hz)
void init_timer(unsigned int frequency) {
    unsigned int divisor = 1193180 / frequency;  // PIT input clock
    port_byte_out(0x43, 0x36);                  // Channel 0, lobyte/hibyte, rate generator
    port_byte_out(0x40, (unsigned char)(divisor & 0xFF));       // Low byte
    port_byte_out(0x40, (unsigned char)((divisor >> 8) & 0xFF)); // High byte
}

// Kernel logging with color-coded levels
void klog(log_level_t level, const char* message) {
    kprint("        ");
    switch (level) {
    case LOG_OK:
        kprint_color("[  OK  ] ", COLOR_LIGHT_GREEN);
        break;
    case LOG_WARN:
        kprint_color("[ WARN ] ", COLOR_LIGHT_BROWN);
        break;
    case LOG_ERROR:
        kprint_color("[ERROR ] ", COLOR_LIGHT_RED);
        break;
    case LOG_FAIL:
        kprint_color("[ FAIL ] ", COLOR_LIGHT_RED);
        break;
    }
    kprint((char*)message);
    kprint("\n");
}

// Swap I/O callbacks — read from block device (LBA addressing)
static int swap_disk_read(uint32_t lba, void* buf, uint32_t sectors)
{
    uint8_t* ptr = (uint8_t*)buf;
    for (uint32_t i = 0; i < sectors; i++) {
        blkdev_read_sector(lba + i, ptr);
        ptr += 512;  // Sector size
    }
    return 0;
}

// Swap I/O callbacks — write to block device
static int swap_disk_write(uint32_t lba, const void* buf, uint32_t sectors)
{
    const uint8_t* ptr = (const uint8_t*)buf;
    for (uint32_t i = 0; i < sectors; i++) {
        blkdev_write_sector(lba + i, (uint8_t*)ptr);
        ptr += 512;
    }
    return 0;
}

// Main hardware initialisation sequence
void kernel_setup_hardware(multiboot_info_t *mbi, mb2_mmap_table_t *mmap) {
    // Memory management (order matters!)
    init_gdt();                     // Global Descriptor Table
    pmm_init_from_mmap(mmap);       // Physical Memory Manager
    init_memory_manager();          // Virtual memory manager
    init_heap();                    // Kernel heap (kmalloc)
    init_paging();                  // Enable paging, load page directory
    slab_init();                    // Slab allocator for kernel objects
    page_fault_init();              // Page fault handler

    // Interrupts
    init_pic();                     // Programmable Interrupt Controller
    init_idt();                     // Interrupt Descriptor Table

    serial_init();                  // COM1 — kprint/klog also go here (QEMU: -serial stdio)

    // Display
    init_framebuffer();

    // Terminal window size from framebuffer geometry
    {
        uint32_t fb_w = fb_get_width();
        uint32_t fb_h = fb_get_height();
        terminal_winsize.ws_col    = (uint16_t)(fb_w / FB_CONSOLE_CHAR_WIDTH);
        terminal_winsize.ws_row    = (uint16_t)(fb_h / FB_CONSOLE_CHAR_HEIGHT);
        terminal_winsize.ws_xpixel = (uint16_t)fb_w;
        terminal_winsize.ws_ypixel = (uint16_t)fb_h;
    }

    // Input devices
    ps2_keyboard_init();
    ps2_mouse_init();

    // Diagnostics
    {
        kprint("[IO] probing PS/2 controller (port 0x64)\n");
        int io_status = probe_io_ports();
        if (io_status)
            klog(LOG_WARN, "port 0x64 = 0xFF — PS/2 controller absent or unresponsive");
        else
            klog(LOG_OK, "PS/2 controller present");
    }

    {
        kprint("[CMOS] reading extended memory from CMOS (regs 0x17/0x18)\n");
        int mem_status = detect_memory();
        if (mem_status)
            klog(LOG_WARN, "CMOS returned 0 KB — memory size unreliable");
        else
            klog(LOG_OK, "CMOS memory size valid");
    }

    /* PIT before PCI scan: GDD prompts use timer_ticks + IRQ keyboard while interrupts stay globally masked until init(). */
    kprint("[PIT] configuring 8253 timer  divisor=");
    { char buf[8]; itoa(1193180 / 100, buf); kprint(buf); }
    kprint("  freq=100 Hz\n");
    init_timer(100);
    klog(LOG_OK, "PIT timer @ 100 Hz — IRQ0 active");

    // Block device layer — must exist BEFORE PCI enumeration so NVMe/AHCI
    // kmods can blkdev_register(); otherwise mntfs sees no boot disk.
    blkdev_init();

    // PCI enumeration and drivers
    kprint("[PCI] scanning bus for devices\n");
    if (search_pci())
        klog(LOG_WARN, "PCI scan reported error");
    else
        klog(LOG_OK, "PCI bus scan complete");

    kprint("[PCI] enumerating and binding drivers\n");
    pci_enumerate();
    {
        char buf[8]; itoa(pci_device_count, buf);
        kprint("[PCI] "); kprint(buf); kprint(" device(s) found\n");
        if (pci_device_count > 0)
            klog(LOG_OK,  "PCI enumeration done");
        else
            klog(LOG_WARN, "no PCI devices — storage/net/USB unavailable");
    }

    // USB xHCI stack
    kprint("[USB] initializing xHCI host controller stack\n");
    extern void usb_init(void);
    usb_init();
    klog(LOG_OK, "USB stack initialised");

    // Process control block cache
    pc_init();

    // Swap subsystem (disk-backed virtual memory)
    {
        int swap_status = swap_init(swap_disk_read, swap_disk_write, 0);
        if (swap_status)
            klog(LOG_WARN, "swap init failed — OOM killer is last resort");
    }

    // Virtual filesystem (mntfs_init is deferred — needs the scheduler).
    vfs_init();

    // Network stack
    net_init();
    // Driver bootstrap is platform policy, not net stack policy.
    // Stack itself accepts any NIC via net_register_driver().
    kprint("[NET] no built-in NIC driver — as root run: "
           "modload /proc/bin/mdls/virtio_net.cctk 0x1AF4 0x1041 (legacy virtio-net; DID 0x1000 also)\n");
    kprint("[BLKDEV] no built-in SATA driver — as root run: "
           "modload /proc/bin/mdls/ahci.cctk (manifest binds class 01:06)\n");
    kprint("[BLKDEV] no built-in NVMe driver — as root run: "
           "modload /proc/bin/mdls/nvme.cctk (manifest binds class 01:08)\n");

    // Multitasking
    task_init();
    init_scheduler();
    pci_driver_probe_deferred_all();
}

extern mb2_module_info_t mb2_cctkfs_module;
#include "pci_modblob.h"

/* Bootstrap thread — runs AFTER scheduler is live, so it can legitimately
 * sleep on semaphores when waiting for IRQ-driven NVMe/AHCI completions
 * during ext4 mount. Must NOT be executed from the boot context (which
 * has been claimed by the idle task and cannot be parked on a semaphore
 * because there is no other task to switch to). */
static multiboot_info_t* bootstrap_mbi = 0;
static void kernel_bootstrap_main(void) {
    extern void mntfs_init(void);
    extern void procfs_set_meminfo(uint32_t);

    kprint("[MNT] mounting virtual filesystems\n");
    mntfs_init();

    if (bootstrap_mbi) {
        char buf[12];
        if (bootstrap_mbi->flags & 0x1) {
            itoa((int)bootstrap_mbi->mem_lower, buf);
            kprint("[MNT] mem_lower="); kprint(buf); kprint(" KB");
            itoa((int)bootstrap_mbi->mem_upper, buf);
            kprint("  mem_upper="); kprint(buf); kprint(" KB\n");
        }
        uint32_t total_kb;
        if (bootstrap_mbi->flags & (1u << 6)) {
            total_kb = (uint32_t)(bootstrap_mbi->mem_total_bytes / 1024ull);
            itoa((int)total_kb, buf);
            kprint("[MNT] mmap total="); kprint(buf); kprint(" KB\n");
        } else {
            total_kb = bootstrap_mbi->mem_lower + 1024 + bootstrap_mbi->mem_upper;
        }
        procfs_set_meminfo(total_kb);
    }
    klog(LOG_OK, "mntfs bootstrap finished");

    kprint("\n");
    kprint_color("Cact Kernel ", COLOR_LIGHT_BROWN);
    kprint_color((char*)kernel_version, COLOR_LIGHT_BROWN);
    kprint_color("\n", COLOR_LIGHT_BROWN);
    kprint_color("--------------------------\n", COLOR_DARK_GREY);
    kprint("[VER] commit="); kprint((char*)kernel_commit_hash);
    kprint("  built=");      kprint((char*)kernel_build_time);
    kprint("\n");

    kprint_color("Kernel is ready. Launching init...\n", COLOR_LIGHT_GREEN);

    struct task_struct* init = create_elf_task("bin/init");
    if (!init) {
        kprint_color("[FAIL] create_elf_task: /bin/init not found\n", COLOR_LIGHT_RED);
    }

    /* Bootstrap thread is done. Yield forever so scheduler keeps running. */
    while (1) {
        __asm__ __volatile__("hlt");
    }
}

// Kernel entry point (called from boot.S)
void init(uint32_t magic, uint32_t mb2_info_addr) {
    static multiboot_info_t  mbi_storage;
    static mb2_mmap_table_t  mmap_storage;
    multiboot2_parse(mb2_info_addr, &mbi_storage, &mmap_storage);
    multiboot_info_t* mbi = &mbi_storage;

    /* Stage the cctkfs blob into kernel .bss BEFORE pmm_init / init_heap
     * — at this point we are still in flat protected mode without paging,
     * so a plain memcpy from the physical address GRUB chose is safe. */
    pci_modblob_load(mb2_cctkfs_module.mod_start,
                     mb2_cctkfs_module.mod_size);

    // Must initialise framebuffer before any output
    fb_init(mbi);

    // No display → completely blind, just halt
    if (fb_get_width() == 0) {
        while(1) __asm__ __volatile__("hlt");
    }

    clear_screen();

    // Validate multiboot2 signature
    if (magic != MB2_BOOTLOADER_MAGIC) {
        kprint_color("[FAIL] Bad multiboot2 magic (got 0x", COLOR_LIGHT_RED);
        char buf[16];
        hex_to_ascii(magic, buf);
        kprint_color(buf, COLOR_LIGHT_RED);
        kprint_color(", expected 0x36D76289)\n", COLOR_LIGHT_RED);
        while(1) __asm__ __volatile__("hlt");
    }

    kernel_setup_hardware(mbi, &mmap_storage);

    /* Spawn the bootstrap kernel thread. It will run mntfs_init, mount ext4
     * (which sleeps on NVMe IRQ via sema_down — only legal from a real task,
     * not from the boot context that was claimed by idle), then start
     * /bin/init. The boot context becomes the idle task (HLT loop below),
     * so the scheduler always has something to fall back to. */
    bootstrap_mbi = mbi;
    create_task((void(*)())kernel_bootstrap_main);

    /* Enable IRQs and surrender to the scheduler. The next timer tick will
     * preempt this context (saved into idle's task_struct) and switch to
     * the bootstrap thread. */
    __asm__ __volatile__("sti");

    while (1) {
        __asm__ __volatile__("hlt");
    }
}