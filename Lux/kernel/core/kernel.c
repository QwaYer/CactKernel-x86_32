#include "kernel.h"
#include "keyboard.h"
#include "memory.h"
#include "gdt.h"
#include "idt.h"
#include "vfs.h"
#include "libc.h"
#include "task.h"
#include "shell.h"

int cursor_x = 0;
int cursor_y = 0;  

char* key_buffer;
int buffer_idx = 0;
volatile int system_ready = 0;

void update_hardware_cursor(int x, int y) {
    unsigned short position = y * MAX_COLS + x;
    port_byte_out(0x3D4, 14);
    port_byte_out(0x3D5, (unsigned char)(position >> 8));
    port_byte_out(0x3D4, 15);
    port_byte_out(0x3D5, (unsigned char)(position & 0xFF));
}

void clear_screen() {
    unsigned char* video_memory = (unsigned char*) VIDEO_ADDRESS;
    for (int i = 0; i < MAX_ROWS * MAX_COLS; i++) {
        video_memory[i * 2] = ' '; 
        video_memory[i * 2 + 1] = WHITE_ON_BLACK;
    }
    cursor_x = 0;
    cursor_y = 0;
    update_hardware_cursor(cursor_x, cursor_y);
}

void scroll() {
    unsigned char* video_memory = (unsigned char*) VIDEO_ADDRESS;
    for (int i = 1; i < MAX_ROWS; i++) {
        for (int j = 0; j < MAX_COLS * 2; j++) {
            video_memory[((i - 1) * MAX_COLS * 2) + j] = video_memory[(i * MAX_COLS * 2) + j];
        }
    }
    for (int j = 0; j < MAX_COLS * 2; j += 2) {
        video_memory[((MAX_ROWS - 1) * MAX_COLS * 2) + j] = ' ';
        video_memory[((MAX_ROWS - 1) * MAX_COLS * 2) + j + 1] = WHITE_ON_BLACK;
    }
}

void kprint_color(char* message, uint8_t color) {
    uint8_t* video_memory = (uint8_t*) VIDEO_ADDRESS;
    for (int i = 0; message[i] != '\0'; i++) {
        if (message[i] == '\n') {
            cursor_x = 0;
            cursor_y++;
        } else {
            int offset = (cursor_y * MAX_COLS + cursor_x) * 2;
            video_memory[offset] = message[i];
            video_memory[offset + 1] = color;
            cursor_x++;
        }

        if (cursor_x >= MAX_COLS) {
            cursor_x = 0;
            cursor_y++;
        }

        if (cursor_y >= MAX_ROWS) {
            scroll();
            cursor_y = MAX_ROWS - 1;
        }
    }
    update_hardware_cursor(cursor_x, cursor_y);
}

void kprint(char* message) {
    kprint_color(message, WHITE_ON_BLACK);
}

void kprint_at(char* message, int x, int y) {
    cursor_x = x;
    cursor_y = y;
    kprint(message);
}


int init_vga() {
    unsigned char* video_memory = (unsigned char*) VIDEO_ADDRESS;
    unsigned char test_val = 0xAF;
    unsigned char old_val = video_memory[0];
    video_memory[0] = test_val;
    if (video_memory[0] != test_val) return 1;
    video_memory[0] = old_val;
    return 0;
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

int search_pci() {
    port_long_out(0xCF8, 0x80000000);
    return (port_long_in(0xCF8) == 0x80000000) ? 0 : 1;
}

void exception_handler(struct context_frame* regs) {
    char buf[32];
    clear_screen();
    kprint_color("!!! KERNEL PANIC !!!\n", COLOR_LIGHT_RED);

    kprint("Exception ID: ");
    itoa(regs->int_no, buf);
    kprint_color(buf, COLOR_LIGHT_RED);

    kprint("\nError Code: 0x");
    hex_to_ascii(regs->err_code, buf);
    kprint_color(buf, COLOR_LIGHT_RED);

    kprint("\nEIP (Address): 0x");
    hex_to_ascii(regs->eip, buf);
    kprint_color(buf, COLOR_LIGHT_RED);

    /* основные регистры для диагностики */
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


void terminal_task() {

    char* cmd_buffer = (char*)kmalloc(1024);
    int idx = 0;

    while (!system_ready);
    

    kprint("\n");
    kprint_color("LuxOS Shell", COLOR_LIGHT_CYAN);
    kprint(" ready!\n");
    kprint_color("qwayer", COLOR_LIGHT_GREEN);
    kprint("@");
    kprint_color("luxos", COLOR_LIGHT_RED);
    kprint(":");
    kprint_color("~", COLOR_LIGHT_BLUE);
    kprint("$ ");

    while (1) {
        if (key_event_happened) {
            key_event_happened = 0;
            char key = last_char;

            if (key == '\n') {
                cmd_buffer[idx] = '\0';

                shell_execute(cmd_buffer); 
                
                idx = 0; 
                kprint("\n");
                kprint_color("qwayer", COLOR_LIGHT_GREEN);
                kprint("@");
                kprint_color("luxos", COLOR_LIGHT_RED);
                kprint(":");
                kprint_color("~", COLOR_LIGHT_BLUE);
                kprint("$ ");
            } 
            else if (key == '\b' && idx > 0) {
                idx--;
                backspace_visual_update();
            } 
            else if (idx < 1024 - 1 && key >= 32) { 
                cmd_buffer[idx++] = key;
                char temp[2] = {key, 0};
                kprint(temp);
            }
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

void kernel_setup_hardware() {
    init_gdt();
    init_memory_manager();
    init_heap();
    init_paging();
    boot_log("Memory Manager & Heap", 0);


    init_pic(); 
    init_idt(); 
    boot_log("IDT & PIC (Interrupts)", 0);

    boot_log("VGA Text Mode", init_vga());
    boot_log("PS/2 Keyboard", init_keyboard());
    boot_log("I/O Ports Probe", probe_io_ports());
    boot_log("Base Memory Detect", detect_memory());
    boot_log("PCI Bus Scan", search_pci());

    ata_init();
    boot_log("ATA Hard Drive", 0);

    ext4_init();
    boot_log("EXT4 File System", 0);

    net_init();
    boot_log("Network Stack", 0);

    task_init();     
    init_scheduler();
    init_timer(100); 
    boot_log("Scheduler & PIT Timer", 0);
    
    key_buffer = (char*)kmalloc(2048);
    if (key_buffer != 0) {
        boot_log("System Buffers", 0);
    } else {
        static char key_buffer_fallback[2048];
        key_buffer = key_buffer_fallback;
        boot_log("System Buffers (fallback)", 1);
    }
}

void init() {
    clear_screen();
    kprint_color("LuxOS Kernel Version 0.0.8\n", COLOR_LIGHT_BROWN);
    kprint_color("--------------------------\n", COLOR_DARK_GREY);

    kernel_setup_hardware();

    kprint("\nSystem is ready. Starting terminal...\n");

        system_ready = 1;
    __asm__ __volatile__("sti");
    while (1) { 
        net_poll();
        __asm__ __volatile__("pause"); 
    }
}
