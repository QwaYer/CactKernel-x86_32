#include "kernel.h"
#include "pci.h"
#include "keyboard.h"
#include "mouse.h"
#include "memory.h"
#include "gdt.h"
#include "idt.h"
#include "vfs.h"
#include "devfs.h"
#include "libc.h"
#include "task.h"
#include "shell.h"
#include "fb.h"
#include "font.h"
#include "swap.h"

extern uint32_t page_directory[1024];

#define FONT_SCALE 2
#define CHAR_W (FONT_WIDTH  * FONT_SCALE)
#define CHAR_H (FONT_HEIGHT * FONT_SCALE)

int cursor_x = 0;
int cursor_y = 0;

char* key_buffer;
int buffer_idx = 0;
volatile int system_ready = 0;

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

int init_framebuffer() {
    return (fb_get_width() > 0) ? 0 : 1;
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
        kprint_color("\n[KERNEL] User process crashed. Exception ID: ", COLOR_LIGHT_RED);
        itoa(regs->int_no, buf);
        kprint_color(buf, COLOR_LIGHT_RED);
        kprint_color(" PID: ", COLOR_LIGHT_RED);
        itoa(current_task->pid, buf);
        kprint_color(buf, COLOR_LIGHT_RED);
        kprint("\n");

        task_signal(current_task->pid, SIGKILL);
        schedule();
        return;
    }

    clear_screen();
    kprint_color("!!! KERNEL FATAL ERROR !!!\n", COLOR_LIGHT_RED);

    kprint("Exception ID: ");
    itoa(regs->int_no, buf);
    kprint_color(buf, COLOR_LIGHT_RED);

    kprint("\nError Code: 0x");
    hex_to_ascii(regs->err_code, buf);
    kprint_color(buf, COLOR_LIGHT_RED);

    kprint("\nEIP (Address): 0x");
    hex_to_ascii(regs->eip, buf);
    kprint_color(buf, COLOR_LIGHT_RED);

    kprint("\nEAX: 0x"); hex_to_ascii(regs->eax, buf); kprint(buf);
    kprint("  EBX: 0x"); hex_to_ascii(regs->ebx, buf); kprint(buf);
    kprint("\nECX: 0x"); hex_to_ascii(regs->ecx, buf); kprint(buf);
    kprint("  EDX: 0x"); hex_to_ascii(regs->edx, buf); kprint(buf);
    kprint("\nESI: 0x"); hex_to_ascii(regs->esi, buf); kprint(buf);
    kprint("  EDI: 0x"); hex_to_ascii(regs->edi, buf); kprint(buf);
    kprint("\nEBP: 0x"); hex_to_ascii(regs->ebp, buf); kprint(buf);

    kprint("\nEFLAGS: 0x");
    hex_to_ascii(regs->eflags, buf); kprint(buf);

    kprint_color("\nSystem Halted.", COLOR_LIGHT_RED);
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

void boot_log(char* component, int status) {
    kprint("  [ ] ");
    kprint(component);

    int len = strlen(component);
    for (int i = 0; i < 35 - len; i++) {
        kprint(".");
    }

    if (status == 0) {
        kprint(" [  ");
        kprint_color("OK", COLOR_LIGHT_GREEN);
        kprint("  ]\n");
    } else {
        kprint(" [ ");
        kprint_color("FAIL", COLOR_LIGHT_RED);
        kprint(" ]\n");
    }
}

int get_cursor_x() { return cursor_x; }
int get_cursor_y() { return cursor_y; }

/* =========================================================================
 * swap I/O wrappers — адаптируют ATA API к сигнатуре swap_read/write_fn
 *
 * ata_read_sector / ata_write_sector работают с одним сектором за раз,
 * поэтому оборачиваем цикл.  Используем Secondary канал, master-диск
 * (port=0x170, slave=0) как swap-раздел.  Поменяйте под свою конфигурацию.
 * ========================================================================= */
#define SWAP_ATA_PORT  0x1F0   /* Primary канал                             */
#define SWAP_ATA_SLAVE 0       /* Master диск                               */

static int swap_disk_read(uint32_t lba, void* buf, uint32_t sectors)
{
    uint8_t* ptr = (uint8_t*)buf;
    for (uint32_t i = 0; i < sectors; i++) {
        ata_read_sector(SWAP_ATA_PORT, SWAP_ATA_SLAVE, lba + i, ptr);
        ptr += 512;
    }
    return 0;
}

static int swap_disk_write(uint32_t lba, const void* buf, uint32_t sectors)
{
    const uint8_t* ptr = (const uint8_t*)buf;
    for (uint32_t i = 0; i < sectors; i++) {
        ata_write_sector(SWAP_ATA_PORT, SWAP_ATA_SLAVE, lba + i, (uint8_t*)ptr);
        ptr += 512;
    }
    return 0;
}

void kernel_setup_hardware() {
    init_gdt();
    init_memory_manager();
    init_heap();

    {
        uint32_t fb_phys = (uint32_t)fb_get_buffer();
        if (fb_phys != 0) {
            uint32_t fb_size = fb_get_height() * fb_get_pitch();
            fb_size = (fb_size + 0xFFF) & ~0xFFF;
            for (uint32_t off = 0; off < fb_size; off += 0x1000) {
                vmm_map(page_directory, fb_phys + off, fb_phys + off,
                        PAGE_PRESENT | PAGE_RW);
            }
        }
    }

    init_paging();
    slab_init();
    boot_log("Memory Manager & Heap", 0);

    page_fault_init();
    boot_log("Page Fault Handler", 0);

    init_pic();
    init_idt();
    boot_log("IDT & PIC (Interrupts)", 0);

    boot_log("Framebuffer", init_framebuffer());
    boot_log("PS/2 Keyboard", init_keyboard());
    init_mouse();
    boot_log("PS/2 Mouse", 0);
    boot_log("I/O Ports Probe", probe_io_ports());
    boot_log("Base Memory Detect", detect_memory());
    boot_log("PCI Bus Scan", search_pci());
    pci_dump_all();

    ata_init();
    boot_log("ATA Hard Drive", 0);
    {
        int swap_status = swap_init(swap_disk_read, swap_disk_write, 0);
        boot_log("Swap (Page Eviction)", swap_status);
    }

    vfs_init();
    boot_log("Virtual File System mount", 0);

    extern void mntfs_init();
    mntfs_init();
    boot_log("EXT4 File System mount", 0);
    boot_log("Device File System mount", 0);
    boot_log("Process File System mount", 0);
    boot_log("Mount manager", 0);

    net_init();
    boot_log("Network Stack", 0);

    task_init();
    init_scheduler();
    init_timer(100);
    boot_log("Scheduler & PIT Timer", 0);

    extern void commands_init();
    commands_init();
    boot_log("Terminal Commands", 0);

    extern void shell_init();
    shell_init();

    key_buffer = (char*)kmalloc(2048);
    if (key_buffer != 0) {
        boot_log("System Buffers", 0);
    } else {
        static char key_buffer_fallback[2048];
        key_buffer = key_buffer_fallback;
        boot_log("System Buffers (fallback)", 1);
    }
}

void init(uint32_t magic, multiboot_info_t* mbi) {
    fb_init(mbi);

    if (fb_get_width() == 0) {
        while(1) __asm__ __volatile__("hlt");
    }

    clear_screen();
    kprint_color("Lux Kernel Version 0.1.0\n", COLOR_LIGHT_BROWN);
    kprint_color("--------------------------\n", COLOR_DARK_GREY);

    if (magic != 0x2BADB002) {
        kprint_color("ERROR: Bad multiboot magic!\n", COLOR_LIGHT_RED);
        while(1) __asm__ __volatile__("hlt");
    }

    kernel_setup_hardware();

    kprint("\nKernel is ready. Starting terminal...\n");

    system_ready = 1;
    __asm__ __volatile__("sti");
    while (1) {
        __asm__ __volatile__("pause");
    }
}