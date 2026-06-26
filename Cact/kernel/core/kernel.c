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
#include "msi.h"
#include "sym.h"

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

// ACPI SCI callback pointer — set by AcpiOsInstallInterruptHandler
void (*acpi_sci_callback)(void) = NULL;

// CPU exception handler — signals for user tasks, panic for kernel
void dump_context_frame(struct context_frame* regs, uint32_t fault_addr, uint32_t signal) {
    char buf[32];
    const char* exc_names[32] = {
        "DE", "DB", "NMI", "BP", "OF", "BR", "UD", "NM",
        "DF", "CSO", "TS", "NP", "SS", "GP", "PF", "MF",
        "AC", "MC", "XF", "??", "??", "??", "??", "??",
        "??", "??", "??", "??", "??", "??", "??", "??"
    };

    kprint_color("\n=== ", COLOR_LIGHT_RED);
    if (signal && current_task && !current_task->is_kernel) {
        kprint_color("SIGNAL ", COLOR_LIGHT_RED);
        hex_to_ascii(signal, buf); kprint_color(buf, COLOR_LIGHT_RED);
        kprint_color(" (pid=", COLOR_LIGHT_RED);
        itoa((int)current_task->pid, buf); kprint_color(buf, COLOR_LIGHT_RED);
        kprint_color(")", COLOR_LIGHT_RED);
    } else {
        kprint_color("PANIC", COLOR_LIGHT_RED);
    }
    kprint_color(" ===\n", COLOR_LIGHT_RED);

    kprint_color("Exception: ", COLOR_LIGHT_RED);
    itoa((int)regs->int_no, buf); kprint(buf);
    kprint(" ("); kprint((char*)exc_names[regs->int_no < 32 ? regs->int_no : 31]);
    kprint(")\n");

    if (regs->int_no == 14) {
        kprint_color("Fault address: 0x", COLOR_LIGHT_RED);
        hex_to_ascii(fault_addr, buf); kprint_color(buf, COLOR_LIGHT_RED);

        uint32_t err = regs->err_code;
        kprint_color("  Error code: 0x", COLOR_LIGHT_RED);
        hex_to_ascii(err, buf); kprint_color(buf, COLOR_LIGHT_RED);
        kprint_color(" [", COLOR_LIGHT_RED);
        if (err & 1) kprint_color("PROT", COLOR_LIGHT_RED);
        else         kprint_color("NP ", COLOR_LIGHT_RED);
        if (err & 2) kprint_color(" W", COLOR_LIGHT_RED);
        else         kprint_color(" R", COLOR_LIGHT_RED);
        if (err & 4) kprint_color(" U", COLOR_LIGHT_RED);
        else         kprint_color(" S", COLOR_LIGHT_RED);
        if (err & 8) kprint(" RSVD");
        if (err & 16) kprint(" IF");
        kprint_color(" ]\n", COLOR_LIGHT_RED);
    } else {
        kprint_color("Error code: 0x", COLOR_LIGHT_RED);
        hex_to_ascii(regs->err_code, buf); kprint_color(buf, COLOR_LIGHT_RED);
        kprint("\n");
    }

    kprint_color(" EIP: 0x", COLOR_LIGHT_RED);
    hex_to_ascii(regs->eip, buf); kprint_color(buf, COLOR_LIGHT_RED);
    kprint_color("  CS: 0x", COLOR_LIGHT_RED);
    hex_to_ascii(regs->cs, buf); kprint_color(buf, COLOR_LIGHT_RED);
    kprint_color(" EFLAGS: 0x", COLOR_LIGHT_RED);
    hex_to_ascii(regs->eflags, buf); kprint_color(buf, COLOR_LIGHT_RED);
    kprint("\n");

    kprint_color("EAX: 0x", COLOR_LIGHT_GREEN);
    hex_to_ascii(regs->eax, buf); kprint_color(buf, COLOR_LIGHT_GREEN);
    kprint_color(" EBX: 0x", COLOR_LIGHT_GREEN);
    hex_to_ascii(regs->ebx, buf); kprint_color(buf, COLOR_LIGHT_GREEN);
    kprint_color(" ECX: 0x", COLOR_LIGHT_GREEN);
    hex_to_ascii(regs->ecx, buf); kprint_color(buf, COLOR_LIGHT_GREEN);
    kprint_color(" EDX: 0x", COLOR_LIGHT_GREEN);
    hex_to_ascii(regs->edx, buf); kprint_color(buf, COLOR_LIGHT_GREEN);
    kprint("\n");

    kprint_color("ESI: 0x", COLOR_LIGHT_GREEN);
    hex_to_ascii(regs->esi, buf); kprint_color(buf, COLOR_LIGHT_GREEN);
    kprint_color(" EDI: 0x", COLOR_LIGHT_GREEN);
    hex_to_ascii(regs->edi, buf); kprint_color(buf, COLOR_LIGHT_GREEN);
    kprint_color(" EBP: 0x", COLOR_LIGHT_GREEN);
    hex_to_ascii(regs->ebp, buf); kprint_color(buf, COLOR_LIGHT_GREEN);
    kprint_color(" ESP: 0x", COLOR_LIGHT_GREEN);
    hex_to_ascii(regs->useresp, buf); kprint_color(buf, COLOR_LIGHT_GREEN);
    kprint("\n");

    kprint_color(" DS: 0x", COLOR_LIGHT_GREEN);
    hex_to_ascii(regs->ds, buf); kprint_color(buf, COLOR_LIGHT_GREEN);
    kprint_color(" ES: 0x", COLOR_LIGHT_GREEN);
    hex_to_ascii(regs->es, buf); kprint_color(buf, COLOR_LIGHT_GREEN);
    kprint_color(" SS: 0x", COLOR_LIGHT_GREEN);
    hex_to_ascii(regs->ss, buf); kprint_color(buf, COLOR_LIGHT_GREEN);
    kprint("\n");

    // Stack trace — walk EBP chain
    kprint_color("Call trace:\n", COLOR_LIGHT_BROWN);
    int frames = 0;
    uint32_t ebp = regs->ebp;
    int is_kernel_fault = (regs->cs == 0x08);

    if (is_kernel_fault) {
        while (ebp >= 0xC0000000 && ebp < 0xC0100000 && frames < 16) {
            uint32_t ret_eip = ((uint32_t*)ebp)[1];
            uint32_t next_ebp = ((uint32_t*)ebp)[0];
            kprint_color("  [", COLOR_LIGHT_BROWN);
            itoa(frames, buf); kprint_color(buf, COLOR_LIGHT_BROWN);
            kprint_color("] 0x", COLOR_LIGHT_BROWN);
            hex_to_ascii(ret_eip, buf); kprint_color(buf, COLOR_LIGHT_BROWN);
            {
                uint32_t sym_off;
                const char* sym = sym_resolve_addr(ret_eip, &sym_off);
                if (sym) {
                    kprint_color(" (", COLOR_DARK_GREY);
                    kprint((char*)sym);
                    kprint_color("+", COLOR_DARK_GREY);
                    hex_to_ascii(sym_off, buf); kprint_color(buf, COLOR_DARK_GREY);
                    kprint_color(")", COLOR_DARK_GREY);
                }
            }
            kprint_color("\n", COLOR_LIGHT_BROWN);
            if (next_ebp <= ebp) break;
            ebp = next_ebp;
            frames++;
        }
    }
    if (frames == 0) {
        kprint_color("  (no trace)\n", COLOR_DARK_GREY);
    }

    // Print EIP instruction bytes
    kprint_color("Code: ", COLOR_LIGHT_BROWN);
    if (is_kernel_fault) {
        uint32_t* pd = is_kernel_fault ? get_current_pd() : current_task->page_directory;
        // Read instruction bytes at EIP
        for (int i = -4; i < 8; i++) {
            uint32_t addr = regs->eip + i;
            if (addr < 0x1000 || addr >= 0xC0000000) { kprint("?? "); continue; }
            uint32_t pdi = PD_INDEX(addr);
            uint32_t pti = PT_INDEX(addr);
            if (!(pd[pdi] & PAGE_PRESENT)) { kprint("?? "); continue; }
            uint32_t* pt = (uint32_t*)(pd[pdi] & ~0xFFFu);
            if (!(pt[pti] & PAGE_PRESENT)) { kprint("?? "); continue; }
            uint32_t phys = (pt[pti] & ~0xFFFu) + (addr & 0xFFFu);
            uint8_t byte = *(volatile uint8_t*)(uintptr_t)phys;
            hex_to_ascii(byte, buf);
            if (i == 0) kprint_color("<", COLOR_LIGHT_GREEN);
            kprint(buf);
            if (i == 0) kprint_color(">", COLOR_LIGHT_GREEN);
            kprint(" ");
        }
    }
    kprint("\n");
}

// CPU exception handler — signals for user tasks, panic for kernel
void exception_handler(struct context_frame* regs) {
    uint32_t signal = 0;
    if (current_task && !current_task->is_kernel) {
        switch (regs->int_no) {
        case 0: case 16: signal = SIGFPE; break;
        case 13: case 14: signal = SIGSEGV; break;
        default: signal = SIGKILL; break;
        }
    }
    dump_context_frame(regs, read_cr2(), signal);

    if (signal && current_task && !current_task->is_kernel) {
        task_signal(current_task->pid, signal);
        schedule();
        return;
    }

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

    // CPU feature detection — before IDT / APIC / PAT so the rest of
    // the kernel can query cpu_vendor(), cpu_has_sep(), etc.
    cpudev_init();

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

    // Program fast-syscall MSRs now that GDT and IDT are ready.
    // IA32_SYSENTER_ESP must be set per-task by the scheduler later.
    cpu_syscall_commit();

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

    msix_init();

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
    klog(LOG_OK, "Hardware setup complete — enabling interrupts / scheduler next");
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