#include "kernel.h"
#include "pci.h"
#include "pci_enum.h"
#include "pci_driver.h"
#include "ps_2_keyboard.h"
#include "ps_2_mouse.h"
#include "memory.h"
#include "gdt.h"
#include "idt.h"
#include "vfs.h"
#include "devfs.h"
#include "klib.h"
#include "task.h"
#include "shell.h"
#include "fb.h"
#include "font.h"
#include "swap.h"
#include "pagecache.h"
#include "blkdev.h"
#include "version.h"

extern uint32_t page_directory[1024];

#define FONT_SCALE 2
#define CHAR_W (FONT_WIDTH  * FONT_SCALE)
#define CHAR_H (FONT_HEIGHT * FONT_SCALE)

int cursor_x = 0;
int cursor_y = 0;

char* key_buffer;
int buffer_idx = 0;
volatile int system_ready = 0;

struct winsize terminal_winsize;
uint32_t       terminal_fg_pid = 0;

static void fb_draw_char_scaled(char c, int px, int py, uint32_t color) {
    if ((unsigned char)c >= 128) return;
    const uint8_t* glyph = font8x8_basic[(unsigned char)c];
    for (int row = 0; row < FONT_HEIGHT; row++) {
        for (int col = 0; col < FONT_WIDTH; col++) {
            uint32_t pix = (glyph[row] & (1 << col)) ? color : COLOR_BLACK;
            for (int sy = 0; sy < FONT_SCALE; sy++)
                for (int sx = 0; sx < FONT_SCALE; sx++)
                    fb_put_pixel(px + col * FONT_SCALE + sx,
                                 py + row * FONT_SCALE + sy, pix);
        }
    }
}

void clear_screen() {
    fb_clear(COLOR_BLACK);
    cursor_x = 0;
    cursor_y = 0;
}

void scroll() {
    uint32_t w = fb_get_width();
    uint32_t h = fb_get_height();
    uint32_t pitch = fb_get_pitch();
    uint32_t* buf = fb_get_buffer();
    if (!buf || w == 0 || h == 0) return;

    uint32_t words_per_row = pitch / 4;
    for (uint32_t y = 0; y + CHAR_H < h; y++)
        for (uint32_t x = 0; x < w; x++)
            buf[y * words_per_row + x] = buf[(y + CHAR_H) * words_per_row + x];

    fb_fill_rect(0, h - CHAR_H, w, CHAR_H, COLOR_BLACK);

    cursor_y -= CHAR_H;
    if (cursor_y < 0) cursor_y = 0;
}

void kprint_color(char* message, uint32_t color) {
    uint32_t w = fb_get_width();
    uint32_t h = fb_get_height();
    if (w == 0 || h == 0) return;

    for (int i = 0; message[i] != '\0'; i++) {
        char c = message[i];
        if (c == '\n') {
            cursor_x = 0;
            cursor_y += CHAR_H;
        } else if (c == '\r') {
            cursor_x = 0;
        } else if (c == '\t') {
            int tab_w = CHAR_W * 4;
            cursor_x = (cursor_x / tab_w + 1) * tab_w;
        } else {
            fb_draw_char_scaled(c, cursor_x, cursor_y, color);
            cursor_x += CHAR_W;
        }

        if (cursor_x + CHAR_W > (int)w) {
            cursor_x = 0;
            cursor_y += CHAR_H;
        }

        if (cursor_y + CHAR_H > (int)h) {
            scroll();
        }
    }
}

void kprint(char* message) {
    kprint_color(message, COLOR_WHITE);
}

void backspace_visual_update() {
    uint32_t w = fb_get_width();
    if (w == 0) return;

    cursor_x -= CHAR_W;
    if (cursor_x < 0) {
        cursor_x = ((int)w / CHAR_W - 1) * CHAR_W;
        cursor_y -= CHAR_H;
        if (cursor_y < 0) { cursor_x = 0; cursor_y = 0; return; }
    }
    fb_fill_rect(cursor_x, cursor_y, CHAR_W, CHAR_H, COLOR_BLACK);
}

void kprint_at(char* message, int x, int y) {
    cursor_x = x;
    cursor_y = y;
    kprint(message);
}

void init_framebuffer() {
    kprint("[FB] checking multiboot framebuffer parameters\n");
    fb_init_result_t status = fb_get_init_status();

    if (status != FB_INIT_OK) {
        static const char* fb_errors[] = {
            [FB_INIT_NO_FLAG]    = "multiboot flag bit 12 not set",
            [FB_INIT_HIGH_ADDR]  = "framebuffer address above 4 GB (not mappable)",
            [FB_INIT_BAD_TYPE]   = "framebuffer type != 1 (not RGB direct-color)",
            [FB_INIT_BAD_BPP]    = "bpp != 32 (only 32-bit color supported)",
            [FB_INIT_NULL_PARAM] = "null address or zero width/height",
        };
        klog(LOG_ERROR, fb_errors[status]);
        klog(LOG_FAIL,  "Framebuffer — cannot continue without display");
        return;
    }

    char buf[16];
    kprint("[FB] addr=0x");
    hex_to_ascii((uint32_t)fb_get_buffer(), buf); kprint(buf);
    kprint("  res=");
    itoa((int)fb_get_width(), buf);  kprint(buf); kprint("x");
    itoa((int)fb_get_height(), buf); kprint(buf);
    kprint("  32bpp  pitch=");
    itoa((int)fb_get_pitch(), buf); kprint(buf); kprint("\n");
    kprint("[FB] cols="); itoa((int)(fb_get_width()  / (8*2)), buf); kprint(buf);
    kprint("  rows=");   itoa((int)(fb_get_height() / (8*2)), buf); kprint(buf); kprint("\n");
    klog(LOG_OK, "Framebuffer ready");
}

int probe_io_ports() {
    if (port_byte_in(0x64) == 0xFF) return 1;
    return 0;
}

int detect_memory() {
    port_byte_out(0x70, 0x17);
    unsigned char low = port_byte_in(0x71);
    port_byte_out(0x70, 0x18);
    unsigned char high = port_byte_in(0x71);
    return ((high << 8) | low) > 0 ? 0 : 1;
}

void exception_handler(struct context_frame* regs) {
    char buf[32];

    if (current_task && !current_task->is_kernel) {
        /* Map CPU exception to the appropriate POSIX signal:
         *   #0  Divide Error          -> SIGFPE
         *   #13 General Protection    -> SIGSEGV
         *   #16 x87 FPU Exception     -> SIGFPE
         *   everything else           -> SIGKILL (unrecoverable) */
        uint32_t sig;
        switch (regs->int_no) {
        case 0:  /* #DE Divide Error */
        case 16: /* #MF x87 FPU Floating-Point Exception */
            sig = SIGFPE;
            kprint_color("\n[KERNEL] User process: FPE (int=", COLOR_LIGHT_RED);
            break;
        case 13: /* #GP General Protection Fault */
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

void init_timer(unsigned int frequency) {
    unsigned int divisor = 1193180 / frequency;
    port_byte_out(0x43, 0x36);
    port_byte_out(0x40, (unsigned char)(divisor & 0xFF));
    port_byte_out(0x40, (unsigned char)((divisor >> 8) & 0xFF));
}

extern uint32_t timer_ticks_get(void);

static void draw_cursor(int visible) {
    uint32_t color = visible ? COLOR_LIGHT_GREY : COLOR_BLACK;
    fb_fill_rect(cursor_x, cursor_y + CHAR_H - 3, CHAR_W, 3, color);
}

void terminal_task() {
    char* cmd_buffer = (char*)kmalloc(1024);
    int idx = 0;

    while (!system_ready);

    /* Compute initial terminal size from framebuffer dimensions */
    uint32_t fb_w = fb_get_width();
    uint32_t fb_h = fb_get_height();
    terminal_winsize.ws_col    = (uint16_t)(fb_w / CHAR_W);
    terminal_winsize.ws_row    = (uint16_t)(fb_h / CHAR_H);
    terminal_winsize.ws_xpixel = (uint16_t)fb_w;
    terminal_winsize.ws_ypixel = (uint16_t)fb_h;

    kprint("\n");
    kprint_color("Lux Shell", COLOR_LIGHT_CYAN);
    kprint(" ready!\n");
    kprint_color("kernel", COLOR_LIGHT_GREEN);
    kprint("@");
    kprint_color("lux", COLOR_LIGHT_RED);
    kprint(":");
    kprint_color("~", COLOR_LIGHT_BLUE);
    kprint("$ ");

    uint32_t last_blink = 0;
    int cursor_visible  = 1;
    draw_cursor(1);

    while (1) {
        uint32_t ticks = timer_ticks_get();
        if (ticks - last_blink >= 50) {
            last_blink = ticks;
            cursor_visible = !cursor_visible;
            draw_cursor(cursor_visible);
        }

        if (key_event_happened) {
            char key = last_char;
            key_event_happened = 0;

            draw_cursor(0);

            if (key == '\n') {
                cmd_buffer[idx] = '\0';
                kprint("\n");
                shell_execute(cmd_buffer);
                idx = 0;
                kprint_color("kernel", COLOR_LIGHT_GREEN);
                kprint("@");
                kprint_color("lux", COLOR_LIGHT_RED);
                kprint(":");
                kprint_color("~", COLOR_LIGHT_BLUE);
                kprint("$ ");
            } else if (key == '\b' && idx > 0) {
                idx--;
                backspace_visual_update();
            } else if (idx < 1024 - 1 && key >= 32) {
                cmd_buffer[idx++] = key;
                char temp[2] = {key, 0};
                kprint(temp);
            }

            cursor_visible = 1;
            last_blink = timer_ticks_get();
            draw_cursor(1);
        }
    }
    kfree_heap(cmd_buffer);
}

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

int get_cursor_x() { return cursor_x; }
int get_cursor_y() { return cursor_y; }

static int swap_disk_read(uint32_t lba, void* buf, uint32_t sectors)
{
    uint8_t* ptr = (uint8_t*)buf;
    for (uint32_t i = 0; i < sectors; i++) {
        blkdev_read_sector(lba + i, ptr);
        ptr += 512;
    }
    return 0;
}

static int swap_disk_write(uint32_t lba, const void* buf, uint32_t sectors)
{
    const uint8_t* ptr = (const uint8_t*)buf;
    for (uint32_t i = 0; i < sectors; i++) {
        blkdev_write_sector(lba + i, (uint8_t*)ptr);
        ptr += 512;
    }
    return 0;
}

void kernel_setup_hardware(multiboot_info_t *mbi) {
    init_gdt();
    init_memory_manager();
    init_heap();

    {
        uint32_t fb_phys = (uint32_t)fb_get_buffer();
        if (fb_phys != 0) {
            uint32_t fb_size = fb_get_height() * fb_get_pitch();
            fb_size = (fb_size + 0xFFF) & ~0xFFF;
            char buf[16];
            kprint("[KRNL] mapping framebuffer 0x");
            hex_to_ascii(fb_phys, buf); kprint(buf);
            kprint("  size="); itoa((int)(fb_size / 1024), buf); kprint(buf); kprint(" KB\n");
            for (uint32_t off = 0; off < fb_size; off += 0x1000)
                vmm_map(page_directory, fb_phys + off, fb_phys + off,
                        PAGE_PRESENT | PAGE_RW);
            klog(LOG_OK, "framebuffer identity-mapped");
        } else {
            klog(LOG_WARN, "framebuffer not yet mapped — skipping VMM map");
        }
    }

    init_paging();
    slab_init();
    page_fault_init();

    init_pic();
    init_idt();

    init_framebuffer();

    ps2_keyboard_init();
    ps2_mouse_init();

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

    kprint("[USB] initializing xHCI host controller stack\n");
    extern void usb_init(void);
    usb_init();
    klog(LOG_OK, "USB stack initialised");

    blkdev_init();

    pc_init();

    {
        int swap_status = swap_init(swap_disk_read, swap_disk_write, 0);
        if (swap_status)
            klog(LOG_WARN, "swap init failed — OOM killer is last resort");
    }

    vfs_init();

    kprint("[MNT] mounting virtual filesystems\n");
    extern void mntfs_init();
    mntfs_init();

    if (mbi && (mbi->flags & 0x1)) {
        extern void procfs_set_meminfo(uint32_t, uint32_t);
        char buf[12];
        itoa((int)mbi->mem_lower, buf);
        kprint("[MNT] mem_lower="); kprint(buf); kprint(" KB");
        itoa((int)mbi->mem_upper, buf);
        kprint("  mem_upper="); kprint(buf); kprint(" KB\n");
        procfs_set_meminfo(mbi->mem_lower, mbi->mem_upper);
    }
    klog(LOG_OK, "EXT4 / devfs / procfs / mntfs mounted");

    net_init();

    task_init();
    init_scheduler();

    kprint("[PIT] configuring 8253 timer  divisor=");
    { char buf[8]; itoa(1193180 / 100, buf); kprint(buf); }
    kprint("  freq=100 Hz\n");
    init_timer(100);
    klog(LOG_OK, "PIT timer @ 100 Hz — IRQ0 active");

    kprint("[SHELL] registering built-in commands\n");
    extern void commands_init();
    commands_init();
    klog(LOG_OK, "shell commands registered");

    extern void shell_init();
    shell_init();

    kprint("[KRNL] allocating key input buffer (2 KB)\n");
    key_buffer = (char*)kmalloc(2048);
    if (key_buffer != 0) {
        klog(LOG_OK, "key buffer ready");
    } else {
        static char key_buffer_fallback[2048];
        key_buffer = key_buffer_fallback;
        klog(LOG_WARN, "heap alloc failed — using static fallback buffer");
    }
}

void init(uint32_t magic, multiboot_info_t* mbi) {
    fb_init(mbi);

    if (fb_get_width() == 0) {
        /* No display available — cannot print, just halt */
        while(1) __asm__ __volatile__("hlt");
    }

    clear_screen();

    if (magic != 0x2BADB002) {
        kprint_color("[FAIL] Bad multiboot magic (got 0x", COLOR_LIGHT_RED);
        char buf[16];
        hex_to_ascii(magic, buf);
        kprint_color(buf, COLOR_LIGHT_RED);
        kprint_color(", expected 0x2BADB002)\n", COLOR_LIGHT_RED);
        while(1) __asm__ __volatile__("hlt");
    }

    kernel_setup_hardware(mbi);

    kprint("\n");
    kprint_color("Lux Kernel ", COLOR_LIGHT_BROWN);
    kprint_color((char*)kernel_version, COLOR_LIGHT_BROWN);
    kprint_color("\n", COLOR_LIGHT_BROWN);
    kprint_color("--------------------------\n", COLOR_DARK_GREY);
    kprint("[VER] commit="); kprint((char*)kernel_commit_hash);
    kprint("  built=");      kprint((char*)kernel_build_time);
    kprint("\n");

    kprint_color("Kernel is ready. Starting terminal...\n", COLOR_LIGHT_GREEN);

    system_ready = 1;
    __asm__ __volatile__("sti");
    while (1) {
        __asm__ __volatile__("hlt");
    }
}