#include "kernel.h"
#include "multiboot2.h"
#include "pci.h"
#include "pcidev.h"
#include "pci_enum.h"
#include "pci_driver.h"
#include "pci_gdd.h"
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
#include "pat.h"
#include "cact_acpi.h"
#include "acpi_timer.h"
#include "acpi_hpet.h"
#include "apic.h"

// Kernel page directory (defined in paging.c)
extern uint32_t page_directory[1024];

// Terminal window size for ioctl() calls
struct winsize terminal_winsize;
uint32_t       terminal_fg_pid = 0;

// Read extended memory size from CMOS (regs 0x17/0x18), return 0 if valid
int detect_memory() {
    port_byte_out(0x70, 0x17);
    unsigned char low = port_byte_in(0x71);
    port_byte_out(0x70, 0x18);
    unsigned char high = port_byte_in(0x71);
    return ((high << 8) | low) > 0 ? 0 : 1;
}

// PS/2 keyboard handler (Set 1 scancode → ASCII)
// Required for default QEMU (no xHCI / USB HID). Remove when all platforms
// provide USB HID keyboard.
#include "keyboard.h"
static const char ps2_scancode_ascii[128] = {
    0, 0, '1','2','3','4','5','6','7','8','9','0','-','=',0,0,
    'q','w','e','r','t','y','u','i','o','p','[',']','\n',0,
    'a','s','d','f','g','h','j','k','l',';','\'','`',0,'\\',
    'z','x','c','v','b','n','m',',','.','/',0,'*',0,' '
};
static void ps2_keyboard_handler_minimal(void) {
    uint8_t status = port_byte_in(0x64);
    if (!(status & 0x01)) return;
    uint8_t scancode = port_byte_in(0x60);
    if (scancode < 0x80) {
        char c = ps2_scancode_ascii[scancode];
        if (c) keyboard_post_key(c);
    }
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

void timer_eoi(void) {
    apic_eoi();
}

void irq_master_slave_eoi(void) {
    apic_eoi();
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
    // Randomise stack canary before any function-prologue checks
    extern void stack_guard_init(void);
    stack_guard_init();

    // Memory management (order matters!)
    init_gdt();                     // Global Descriptor Table
    klog(LOG_OK, "GDT initialized");
    pmm_init_from_mmap(mmap);       // Physical Memory Manager
    klog(LOG_OK, "Physical memory manager ready");
    init_memory_manager();          // Virtual memory manager
    klog(LOG_OK, "Virtual memory manager ready");
    init_heap();                    // Kernel heap (kmalloc)
    klog(LOG_OK, "Kernel heap ready");
    init_paging();                  // Enable paging, load page directory
    klog(LOG_OK, "Paging enabled");
    slab_init();                    // Slab allocator for kernel objects
    klog(LOG_OK, "Slab allocator ready");
    page_fault_init();              // Page fault handler
    klog(LOG_OK, "Page fault handler installed");

    // Interrupts
    init_idt();                     // Interrupt Descriptor Table
    klog(LOG_OK, "IDT loaded");
    irq_register_handler(1, ps2_keyboard_handler_minimal);

    serial_init();                  // COM1 — kprint/klog also go here (QEMU: -serial stdio)
    klog(LOG_OK, "Serial console (COM1) ready");

    // Display
    init_framebuffer();

    // Probe PAT support and, if available, mark the framebuffer as Write-
    // Combining via the PAT bit in each PTE. The boot identity map sets
    // PCD|PWT on every page above PCI_HOLE_START (so MMIO registers remain
    // strictly UC).  PAT lets us override individual FB PTEs to WC by
    // setting the PAT bit and clearing PCD|PWT — no MTRR ranges needed.
    pat_init();
    if (fb_get_width() != 0) {
        pat_enable_wc_for_framebuffer((uint32_t)(uintptr_t)fb_get_buffer(),
                                       fb_get_pitch(),
                                       fb_get_height());
        // Back-buffer in WB RAM: kills the FB->FB read penalty in scroll() and
        // coalesces all per-character writes into one rectangular blit per
        // kprint(). Must run AFTER PAT enables WC (the seeding memcpy reads
        // the FB once; under UC this would stall, under WC it's bearable).
        fb_enable_shadow();
        klog(LOG_OK, "Framebuffer WC + shadow buffer configured");
    }

    // Terminal window size from framebuffer geometry
    {
        uint32_t fb_w = fb_get_width();
        uint32_t fb_h = fb_get_height();
        terminal_winsize.ws_col    = (uint16_t)(fb_w / FB_CONSOLE_CHAR_WIDTH);
        terminal_winsize.ws_row    = (uint16_t)(fb_h / FB_CONSOLE_CHAR_HEIGHT);
        terminal_winsize.ws_xpixel = (uint16_t)fb_w;
        terminal_winsize.ws_ypixel = (uint16_t)fb_h;
    }

    // Diagnostics
    {
        int mem_status = detect_memory();
        if (mem_status)
            klog(LOG_WARN, "CMOS returned 0 KB — memory size unreliable");
    }

    // ACPI subsystem (before PCI — HPET/APIC need ACPI tables)
    if (acpi_init())
        klog(LOG_WARN, "ACPI init returned error — hardware limited");
    else
        klog(LOG_OK, "ACPI subsystem ready");

    if (acpi_pm_timer_init() == 0)
        klog(LOG_OK, "ACPI PM timer: timekeeping ready");
    else
        klog(LOG_WARN, "ACPI PM timer unavailable — timekeeping degraded");

    if (hpet_init() == 0)
        klog(LOG_OK, "HPET ready — replacing PIT for timekeeping");
    else
        klog(LOG_WARN, "HPET not available — PIT stays for timekeeping");

    if (apic_init() == 0)
        klog(LOG_OK, "APIC operational");
    else
        klog(LOG_WARN, "APIC init failed — interrupts will not work");

    // Block device layer — must exist BEFORE PCI enumeration so NVMe/AHCI
    // kmods can blkdev_register(); otherwise mntfs sees no boot disk.
    blkdev_init();

    // PCI Express ECAM init (needs ACPI tables, before PCI enumeration)
    pcidev_init();
    {
        if (pci_device_count <= 0)
            klog(LOG_WARN, "no PCI devices — storage/net/USB unavailable");
    }

    // USB xHCI stack
    extern void usb_init(void);
    usb_init();

    // GDD prompts — after USB init so USB HID keyboard works
    pci_gdd_prompt_devices();

    // Process control block cache
    pc_init();

    // Swap subsystem (disk-backed virtual memory)
    {
        int swap_status = swap_init(swap_disk_read, swap_disk_write, 0);
        if (swap_status)
            klog(LOG_WARN, "swap init failed — OOM killer is last resort");
        else
            klog(LOG_OK, "Swap subsystem ready");
    }

    // Virtual filesystem (mntfs_init is deferred — needs the scheduler).
    vfs_init();

    // Network stack
    net_init();

    // Multitasking
    task_init();
    init_scheduler();
    klog(LOG_OK, "Hardware setup complete — enabling IRQs / scheduler next");
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

    pcidev_probe_all();

    // Wire xHCI ISR — on q35 via PCI vector (GSI 16+, apic_pci_vector),
    // on i440fx via shared ISA IRQ (irq_line).
    {
        extern pci_device_t *pci_device_list;
        extern void set_idt_gate(int n, uint32_t handler);
        extern void xhci_irq_handler(void);
        extern void xhci_isr();
        for (pci_device_t *d = pci_device_list; d; d = d->next) {
            if (d->class_code != 0x0C || d->subclass != 0x03 || d->prog_if != 0x30)
                continue;
            // q35 path: PIRQ → GSI 16+ → PCI vector
            int vec = apic_pci_vector(d->irq_pin);
            if (vec > 0)
                set_idt_gate(vec, (uint32_t)xhci_isr);
            // i440fx path: PIRQ → ISA IRQ (shared with other PCI drivers)
            if (d->irq_line > 0 && d->irq_line < 16)
                irq_register_shared_handler(d->irq_line, xhci_irq_handler);
        }
    }

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
    // Validate multiboot2 signature BEFORE using any MB2 data
    if (magic != MB2_BOOTLOADER_MAGIC) {
        while(1) __asm__ __volatile__("hlt");
    }

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