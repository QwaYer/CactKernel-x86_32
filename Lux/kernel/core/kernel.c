#include "kernel.h"
#include "memory.h"
#include "vfs.h"
#include "libc.h"
#include "task.h"

int cursor_x = 0;
int cursor_y = 0;  

struct idt_entry {
    unsigned short low_offset;  
    unsigned short sel;          
    unsigned char always0;
    unsigned char flags;        
    unsigned short high_offset; 
} __attribute__((packed));

struct idt_ptr {
    unsigned short limit;      
    unsigned int base;           
} __attribute__((packed));

struct idt_entry idt[256];
struct idt_ptr idtp;

unsigned char keyboard_map[128] = {
    0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0,
    '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' '
};

unsigned char keyboard_map_shift[128] = {
    0,  27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0,
    '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' '
};

int shift_pressed = 0;
int caps_lock_active = 0;

char* key_buffer; 
int buffer_idx = 0;

volatile char last_char = 0;
volatile int key_event_happened = 0;

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

void kprint(char* message) {
    unsigned char* video_memory = (unsigned char*) VIDEO_ADDRESS;
    int i = 0;
    while (message[i] != 0) {
        if (message[i] == '\n') {
            cursor_x = 0;
            cursor_y++;
        } else {
            int offset = (cursor_y * MAX_COLS + cursor_x) * 2;
            video_memory[offset] = message[i];
            video_memory[offset + 1] = WHITE_ON_BLACK;
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
        i++;
    }
    update_hardware_cursor(cursor_x, cursor_y);
}

void kprint_at(char* message, int x, int y) {
    cursor_x = x;
    cursor_y = y;
    kprint(message);
}

void set_idt_gate(int n, unsigned int handler) {
    idt[n].low_offset = (unsigned short)(handler & 0xFFFF);
    idt[n].sel = 0x08;
    idt[n].always0 = 0;
    idt[n].flags = 0x8E; 
    idt[n].high_offset = (unsigned short)((handler >> 16) & 0xFFFF);
}

void init_pic() {
    port_byte_out(0x20, 0x11);
    port_byte_out(0xA0, 0x11);
    port_byte_out(0x21, 0x20); 
    port_byte_out(0xA1, 0x28); 
    port_byte_out(0x21, 0x04);
    port_byte_out(0xA1, 0x02);
    port_byte_out(0x21, 0x01);
    port_byte_out(0xA1, 0x01);
    port_byte_out(0x21, 0xFC); 
    port_byte_out(0xA1, 0xFF);
}

int init_idt() {
    idtp.limit = (sizeof(struct idt_entry) * 256) - 1;
    idtp.base = (unsigned int)&idt;
    memory_set(&idt, 0, sizeof(struct idt_entry) * 256);

    set_idt_gate(0, (unsigned int)isr0);
    set_idt_gate(13, (unsigned int)isr13);
    set_idt_gate(14, (unsigned int)isr14);

    for (int i = 1; i < 32; i++) {
        if (i != 13 && i != 14) set_idt_gate(i, (unsigned int)isr_common_stub);
    }

    set_idt_gate(32, (unsigned int)timer_isr);
    set_idt_gate(33, (unsigned int)keyboard_isr);

    set_idt_gate(128, (unsigned int)syscall_isr);

    __asm__ __volatile__("lidt (%0)" : : "r" (&idtp));
    return 0;
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

int init_keyboard() {
    while(port_byte_in(0x64) & 0x02);
    port_byte_out(0x64, 0xAE);
    while(port_byte_in(0x64) & 0x01) port_byte_in(0x60);
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

int init_scheduler() { return 0; }

void exception_handler() {
    clear_screen();
    kprint("KERNEL PANIC: CPU Exception!");
    __asm__ __volatile__ ("cli; hlt");
}

void init_timer(unsigned int frequency) {
    unsigned int divisor = 1193180 / frequency;
    port_byte_out(0x43, 0x36);
    port_byte_out(0x40, (unsigned char)(divisor & 0xFF));
    port_byte_out(0x40, (unsigned char)((divisor >> 8) & 0xFF));
}

void keyboard_handler() {
    unsigned char scancode = port_byte_in(0x60);
    if (scancode == 0x2A || scancode == 0x36) shift_pressed = 1;
    else if (scancode == 0xAA || scancode == 0xB6) shift_pressed = 0;
    else if (scancode == 0x3A) caps_lock_active = !caps_lock_active;
    else if (!(scancode & 0x80)) {
        char c = keyboard_map[scancode];
        if (c >= 'a' && c <= 'z' && (shift_pressed != caps_lock_active)) c = keyboard_map_shift[scancode];
        else if (shift_pressed && !(c >= 'a' && c <= 'z')) c = keyboard_map_shift[scancode];
        
        if (c != 0) {
            last_char = c;
            key_event_happened = 1; 
        }
    }
    port_byte_out(0x20, 0x20); 
}

void execute_command(char* input) {
    if (compare_string(input, "clear") == 0) {
        clear_screen();
    } 
    else if (compare_string(input, "help") == 0) {
        kprint("\nLuxOS Commands: help, clear, ls, cpu, mem, af, cat, df\n");
    }
    else if (compare_string(input, "cpu") == 0) {
        kprint("\nCPU: x86 32-bit Protected Mode\nMultitasking: ENABLED\n");
    }
    else if (compare_string(input, "ls") == 0) {
        kprint("\nRoot Directory:");
        fat16_list_root();
    }
    else if (input[0] == 'c' && input[1] == 'a' && input[2] == 't') {
        struct vfs_node* file = fat16_finddir(vfs_root, input + 4);
        if (file) {
            char* buf = (char*)kmalloc(2048);
            int read = file->read(file, 0, file->size, buf);
            buf[read] = '\0';
            kprint("\n"); kprint(buf);
            kfree_heap(buf); kfree_heap(file);
        } else kprint("\nFile not found.");
    }
    else {
        kprint("\nUnknown command: "); kprint(input);
    }
    kprint("\n> ");
}


void terminal_task() {
    kprint("\nLuxOS Terminal is ready.\n> ");
    while (1) {
        if (key_event_happened) {
            key_event_happened = 0; 
            char key = last_char;

            if (key == '\n') {
                key_buffer[buffer_idx] = '\0';
                execute_command(key_buffer);
                buffer_idx = 0; 
            } 
            else if (key == '\b') {
                if (buffer_idx > 0) {
                    buffer_idx--;
                    if (cursor_x > 2) { 
                        cursor_x--;
                        unsigned char* video_memory = (unsigned char*) VIDEO_ADDRESS;
                        int offset = (cursor_y * MAX_COLS + cursor_x) * 2;
                        video_memory[offset] = ' '; 
                        update_hardware_cursor(cursor_x, cursor_y);
                    }
                }
            } 
            else if (buffer_idx < 2048 - 1) { 
                key_buffer[buffer_idx++] = key;
                char temp_str[2] = {key, 0};
                kprint(temp_str);
            }
        }
        __asm__ __volatile__("hlt");
    }
}


void init() {
    clear_screen();
    kprint("LuxOS Kernel 0.0.7 Initializing...\n");

    init_memory_manager();
    init_heap();
    key_buffer = (char*)kmalloc(2048);

    init_pic(); 
    init_idt(); 
    
    detect_memory();
    init_vga();
    init_keyboard();
    ata_init();
    fat16_init();

    init_scheduler();
    init_timer(100); 

    current_task = create_task(terminal_task); 

    kprint("System ready. Enabling interrupts...\n");
    __asm__ __volatile__("sti");

    while (1) {
        __asm__ __volatile__("hlt");
    }
}