#include "kernel.h"
#include "multiboot2.h"
#include "pci.h"
#include "pcidev.h"
#include "pci_enum.h"
#include "pci_driver.h"
#include "memory.h"
#include "gdt.h"
#include "idt.h"
#include "vfs.h"
#include "fs_mod.h"
#include "devfs.h"
#include "klib.h"
#include "task.h"
#include "fb.h"
#include "font.h"
#include "swap.h"
#include "pagecache.h"
#include "blkdev.h"
#include "version.h"
#include "serial.h"
#include "lapic_timer.h"
#include "pat.h"
#include "cact_acpi.h"
#include "acpi_timer.h"
#include "acpi_hpet.h"
#include "apic.h"
#include "msi.h"
#include "initfs_modblob.h"

// Kernel page directory (defined in paging.c)
extern uint32_t page_directory[1024];

// Terminal window size for ioctl() calls
struct winsize terminal_winsize;
uint32_t       terminal_fg_pid = 0;

// ACPI SCI callback pointer — set by AcpiOsInstallInterruptHandler
void (*acpi_sci_callback)(void) = NULL;

// Read extended memory size from CMOS (regs 0x17/0x18), return 0 if valid
int detect_memory() {
    outb(0x70, 0x17);
    unsigned char low = inb(0x71);
    outb(0x70, 0x18);
    unsigned char high = inb(0x71);
    return ((high << 8) | low) > 0 ? 0 : 1;
}

/* The framebuffer console has no built-in font: the PSF2 file must be staged
 * inside the cctkfs archive.  When it is absent or unusable the kernel cannot
 * draw a single glyph, so there is nothing left to do but panic. */
static void boot_font_panic(int rc) __attribute__((noreturn));
static void boot_font_panic(int rc) {
    const char *reason;
    switch (rc) {
    case FONT_ERR_NOT_FOUND:    reason = "console font not found in cctkfs";  break;
    case FONT_ERR_BAD_SIZE:     reason = "console font blob too small";        break;
    case FONT_ERR_NOT_PSF2:     reason = "console font is not a PSF2 file";    break;
    case FONT_ERR_BAD_VERSION:  reason = "console font: unsupported PSF2 version"; break;
    case FONT_ERR_BAD_HEADER:   reason = "console font: malformed PSF2 header"; break;
    case FONT_ERR_GLYPHS:       reason = "console font: glyph data truncated";  break;
    case FONT_ERR_UNICODE:      reason = "console font: bad unicode table";     break;
    default:                    reason = "console font load failed";            break;
    }

    serial_init();
    serial_putc('\n');
    for (const char *s = "*** KERNEL PANIC: "; *s; s++) serial_putc(*s);
    for (const char *s = reason; *s; s++) serial_putc(*s);
    serial_putc('\n');

    uint32_t pw = fb_get_width();
    uint32_t ph = fb_get_height();
    if (pw != 0 && ph != 0)
        fb_fill_rect(0, 0, pw, ph, COLOR_RED);

    for (;;)
        __asm__ __volatile__("hlt");
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

    uint32_t mem_total_kb = 0;
    if (mbi->flags & (1u << 6)) {
        mem_total_kb = (uint32_t)(mbi->mem_total_bytes / 1024ull);
    } else {
        mem_total_kb = mbi->mem_lower + 1024 + mbi->mem_upper;
    }

    // Memory management (order matters!)
    init_gdt();                     // Global Descriptor Table
    pr_info("  %-11s : loaded\n", "gdt");

    // CPU feature detection — before IDT / APIC / PAT so the rest of
    // the kernel can query cpu_vendor(), cpu_has_sep(), etc.
    cpudev_init();

    // Enable the x87 FPU + SSE up front. The compiler emits XMM sequences
    // for 64-bit operations (e.g. ACPI's AcpiOsGetRootPointer returns a
    // UINT64); without CR4.OSFXSR those fault with #UD, and the lazy-FPU
    // fxsave/fxrstor path needs it too. Must be done before anything uses
    // 64-bit locals/returns in C.
    if (fpu_global_init() == 0)
        pr_info("  %-11s : x87 FPU + SSE enabled\n", "fpu/sse");
    else
        pr_warn("  %-11s : x87 only — no SSE operations\n", "fpu/sse");

    pmm_init_from_mmap(mmap);       // Physical Memory Manager
    pr_info("  %-11s : %u MiB usable RAM, frame allocator ready\n",
            "pmm", mem_total_kb / 1024u);
    init_memory_manager();          // Virtual memory manager
    pr_info("  %-11s : virtual address-space manager ready\n", "vmm");
    init_heap();                    // Kernel heap (kmalloc)
    pr_info("  %-11s : kmalloc arena ready (%u KiB free)\n",
            "heap", get_free_heap_memory() / 1024u);
    init_paging();                  // Enable paging, load page directory
    pr_info("  %-11s : enabled\n", "paging");
    slab_init();                    // Slab allocator for kernel objects
    pr_info("  %-11s : kernel object caches ready\n", "slab");
    page_fault_init();              // Page fault handler
    pr_info("  %-11s : #PF handler installed\n", "pagefault");

    // Interrupts
    init_idt();                     // Interrupt Descriptor Table
    pr_info("  %-11s : 256-vector table loaded\n", "idt");

    // Program fast-syscall MSRs now that GDT and IDT are ready.
    // IA32_SYSENTER_ESP is updated per-task by the scheduler on every switch.
    cpu_syscall_commit();

    serial_init();                  // COM1 — printk/klog also go here (QEMU: -serial stdio)
    pr_info("  %-11s : COM1 @ 115200 8N1\n", "serial");

    // Display
    init_framebuffer();

    // Probe PAT support and, if available, mark the framebuffer as Write-
    // Combining via the PAT bit in each PTE. The boot identity map sets
    // PCD|PWT on every page above PCI_HOLE_START (so MMIO registers remain
    // strictly UC).  PAT lets us override individual FB PTEs to WC by
    // setting the PAT bit and clearing PCD|PWT — no MTRR ranges needed.
    pat_init();
    if (fb_get_width() != 0) {
        int fbwc = pat_enable_wc_for_framebuffer(
            (uint32_t)(uintptr_t)fb_get_buffer(),
            fb_get_pitch(),
            fb_get_height());
        // Back-buffer in WB RAM: kills the FB->FB read penalty in scroll() and
        // coalesces all per-character writes into one rectangular blit per
        // printk(). Must run AFTER PAT enables WC (the seeding memcpy reads
        // the FB once; under UC this would stall, under WC it's bearable).
        fb_enable_shadow();
        pr_info("  %-11s : %ux%u @ 32bpp, %s(PAT) + WB shadow ready\n",
                "framebuffer",
                fb_get_width(), fb_get_height(),
                (fbwc == 0) ? "WC" : "UC");
    }

    // Terminal window size from framebuffer geometry
    {
        uint32_t fb_w = fb_get_width();
        uint32_t fb_h = fb_get_height();
        uint32_t cellw = fb_char_cell_w();
        uint32_t cellh = fb_char_cell_h();
        terminal_winsize.ws_col    = (uint16_t)(fb_w / cellw);
        terminal_winsize.ws_row    = (uint16_t)(fb_h / cellh);
        terminal_winsize.ws_xpixel = (uint16_t)fb_w;
        terminal_winsize.ws_ypixel = (uint16_t)fb_h;
    }

    // Diagnostics
    {
        int mem_status = detect_memory();
        if (mem_status)
            pr_warn("  %-11s : CMOS reported 0 KB — size unreliable\n", "memory");
    }

    // ACPI subsystem (before PCI — HPET/APIC need ACPI tables)
    if (acpi_init())
        pr_warn("  %-11s : init failed — hardware limited\n", "acpi");

    /* Success (port/width detail) is reported inside acpi_pm_timer_init. */
    if (acpi_pm_timer_init() != 0)
        pr_warn("  %-11s : unavailable — timekeeping degraded\n", "pm-timer");

    if (lapic_timer_select_source() != 0)
        pr_crit("  %-11s : no usable system timer source\n", "timer");

    if (apic_init() == 0)
        pr_info("  %-11s : LAPIC + IOAPIC operational\n", "apic");
    else
        pr_warn("  %-11s : init failed — interrupts will not work\n", "apic");

    msix_init();

    // Block device layer — must exist BEFORE PCI enumeration so NVMe/AHCI
    // kmods can register_blkdev(); otherwise mntfs sees no boot disk.
    blkdev_init();

    // Partition layer: scans MBR/GPT whenever a disk is registered, so disks
    // probed by storage kmods during the boot sweep and late-loaded kmods are
    // both partitioned. Scanning reads sector 0/1 through the driver, so it
    // must happen from a task context — register_blkdev() is only ever called
    // from storage-driver probes running in the bootstrap/user task.
    extern void part_probe_init(void);
    part_probe_init();

    // PCI Express ECAM init (needs ACPI tables, before PCI enumeration)
    pcidev_init();
    {
        if (pci_device_count <= 0)
            pr_warn("  %-11s : no devices found\n", "pci");
    }

    // USB xHCI stack
    extern void usb_init(void);
    usb_init();

    // Process control block cache
    pc_init();

    // Swap subsystem (disk-backed virtual memory)
    {
        int swap_status = swap_init(swap_disk_read, swap_disk_write, 0);
        if (swap_status)
            pr_warn("  %-11s : init failed — OOM killer is last resort\n", "swap");
        else
            pr_info("  %-11s : disk-backed VM ready\n", "swap");
    }

    // Virtual filesystem (mntfs_init is deferred — needs the scheduler).
    vfs_init();

    // Network stack
    net_init();

    // Multitasking
    task_init();
    init_scheduler();
    pr_info("  %-11s : hardware setup complete — scheduler live\n", "boot");
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
    initfs_modblob_load(mb2_cctkfs_module.mod_start,
                     mb2_cctkfs_module.mod_size);

    // Must initialise framebuffer before any output
    fb_init(mbi);

    // No display → completely blind, just halt
    if (fb_get_width() == 0) {
        while(1) __asm__ __volatile__("hlt");
    }

    // The framebuffer console has no embedded font: parse the PSF2 file
    // staged in the cctkfs image before any glyph is drawn.
    {
        int frc = font_load_boot(CONSOLE_FONT_CCTKFS_PATH);
        if (frc != FONT_OK)
            boot_font_panic(frc);
    }

    clear_screen();

    kernel_setup_hardware(mbi, &mmap_storage);

    /* Spawn the bootstrap kernel thread. It will run mntfs_init, mount ext4
     * (which sleeps on NVMe IRQ via down — only legal from a real task,
     * not from the boot context that was claimed by idle), then start
     * /bin/init. The boot context becomes the idle task (HLT loop below),
     * so the scheduler always has something to fall back to. */
    kernel_spawn_bootstrap(mbi);

    /* Enable IRQs and surrender to the scheduler. The next timer tick will
     * preempt this context (saved into idle's task_struct) and switch to
     * the bootstrap thread. */
    __asm__ __volatile__("sti");

    /* Watchdog: on real hardware a dead system timer silently freezes the
     * machine here (no IRQ ever wakes the idle hlt).  Wait for the first
     * scheduler tick against the ACPI PM timer wall clock; if none comes,
     * arm the LAPIC timer as a last-resort fallback and only then give up —
     * some boards expose an HPET that accepts register writes but never
     * raises its interrupt. */
    {
        extern uint32_t timer_ticks_get(void);
        uint32_t wd_ticks = timer_ticks_get();
        int wd_attempts = 0;

        for (;;) {
            uint64_t wd_usec = acpi_pm_timer_get_usec();
            while (timer_ticks_get() == wd_ticks) {
                __asm__ __volatile__("pause");
                if (acpi_pm_timer_get_usec() - wd_usec < 2000000ull)
                    continue;
                break;   /* 2 s with no scheduler tick */
            }
            if (timer_ticks_get() != wd_ticks)
                break;   /* timer alive — normal boot */

            if (wd_attempts++ < 1) {
                printk_color("  timer       : WARNING — no tick for 2 s, arming LAPIC timer\n",
                             COLOR_LIGHT_RED);
                uint32_t per_ms = lapic_timer_calibrate();
                if (per_ms)
                    lapic_timer_start_periodic(per_ms);
                continue;
            }

            printk_color("  timer       : FATAL — no tick for 2 s (HPET/LAPIC dead), "
                         "scheduler cannot start\n",
                         COLOR_LIGHT_RED);
            while (1) __asm__ __volatile__("hlt");
        }
    }

    while (1) {
        __asm__ __volatile__("hlt");
    }
}
