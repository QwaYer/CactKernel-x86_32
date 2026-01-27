#include "kernel.h"
#include "memory.h"
#include "vfs.h"
#include "fat16.h"

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

int strcmp(char* s1, char* s2) {
    int i;
    for (i = 0; s1[i] == s2[i]; i++) {
        if (s1[i] == '\0') return 0;
    }
    return s1[i] - s2[i];
}

void itoa(int n, char str[]) {
    int i, sign;
    if ((sign = n) < 0) n = -n;
    i = 0;
    do {
        str[i++] = n % 10 + '0';
    } while ((n /= 10) > 0);
    if (sign < 0) str[i++] = '-';
    str[i] = '\0';

    for (int j = 0, k = i - 1; j < k; j++, k--) {
        char temp = str[j];
        str[j] = str[k];
        str[k] = temp;
    }
}

int atoi(char* str) {
    int res = 0, sign = 1, i = 0;
    if (str[0] == '-') { sign = -1; i++; }
    for (; str[i] != '\0'; ++i) {
        if (str[i] < '0' || str[i] > '9') break;
        res = res * 10 + str[i] - '0';
    }
    return sign * res;
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
    port_byte_out(0x21, 0xFD); 
    port_byte_out(0xA1, 0xFF);
}

int init_idt() {
    idtp.limit = (unsigned short)(sizeof(struct idt_entry) * 256) - 1;
    idtp.base = (unsigned int)&idt;
    for (int i = 0; i < 256; i++) set_idt_gate(i, 0);
    init_pic();
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

void keyboard_handler() {
    unsigned char scancode = port_byte_in(0x60);

    if (scancode == 0x2A || scancode == 0x36) {
        shift_pressed = 1;
    } 
    else if (scancode == 0xAA || scancode == 0xB6) {
        shift_pressed = 0;
    } 
    else if (scancode == 0x3A) { 
        caps_lock_active = !caps_lock_active;
    }
    else if (!(scancode & 0x80)) {
        if (scancode < 128) {
            char c = keyboard_map[scancode];

            if (c >= 'a' && c <= 'z') {
                if (shift_pressed != caps_lock_active) {
                    c = keyboard_map_shift[scancode];
                }
            } else if (shift_pressed) {
                c = keyboard_map_shift[scancode];
            }
            
            if (c != 0) {
                last_char = c;
                key_event_happened = 1; 
            }
        }
    }
    port_byte_out(0x20, 0x20); 
}

void execute_command(char* input) {
    if (strcmp(input, "clear") == 0) {
        clear_screen();
    } 
    else if (strcmp(input, "help") == 0) {
        kprint("\nLuxOS Commands:\n");
        kprint("clear - Clear screen\n");
        kprint("ls    - List files\n");
        kprint("af    - Add file (af <name>)\n");
        kprint("wf    - Write to file (wf <name> <text>)\n");
        kprint("cat   - Read file (cat <name>)\n");
        kprint("df    - Delete file (df <name>)\n");
    }
    else if (strcmp(input, "ls") == 0) {
        kprint("\nRoot Directory:");
        fat16_list_root();
    }
    else if (input[0] == 'a' && input[1] == 'f' && input[2] == ' ') {
        if (fat16_create_file(input + 3) == 0) kprint("\nFile created.");
        else kprint("\nError: Could not create.");
    }
    else if (input[0] == 'd' && input[1] == 'f' && input[2] == ' ') {
        if (fat16_delete_file(input + 3) == 0) kprint("\nFile deleted.");
        else kprint("\nError: File not found.");
    }
    else if (input[0] == 'w' && input[1] == 'f' && input[2] == ' ') {
        char* filename = input + 3;
        char* text = 0;
        for (char* p = filename; *p; p++) { 
            if (*p == ' ') { 
                *p = '\0'; 
                text = p + 1; 
                break; 
            } 
        }
        if (text) {
            struct vfs_node* file = fat16_finddir(vfs_root, filename);
            if (file) {
                int len = 0; while(text[len]) len++;
                file->write(file, 0, len, text);
                kprint("\nSaved.");
                kfree_heap(file);
            } else kprint("\nFile not found.");
        }
    }
    else if (input[0] == 'c' && input[1] == 'a' && input[2] == 't' && input[3] == ' ') {
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
        kprint("\nUnknown: "); kprint(input);
    }
    kprint("\n> ");
}

void init() {
    clear_screen();
    kprint("LuxOS Kernel 0.0.7\n");
    kprint("-----------------------------------------\n");

    int current_row = 2;

    init_memory_manager();
    init_heap();

    key_buffer = (char*)kmalloc(2048);
    if (key_buffer == 0) {
        kprint("CRITICAL ERROR: Memory allocation failed!\n");
        while(1);
    }

    kprint_at("Initializing GDT..................", 0, current_row++);
    kprint_at("CPU Protected Mode (x86_32).......", 0, current_row++);
    kprint_at("Setting up Kernel Stack...........", 0, current_row++);

    kprint_at("Initializing IDT & PIC............", 0, current_row);
    if (init_idt() == 0) {
        extern void keyboard_isr();
        set_idt_gate(33, (unsigned int)keyboard_isr);
        kprint_at("[ OK ]", 35, current_row++);
    } else {
        kprint_at("[FAIL]", 35, current_row++);
        while(1);
    }

    kprint_at("Detecting Memory Regions..........", 0, current_row);
    if (detect_memory() == 0) {
        kprint_at("[ OK ]", 35, current_row++);
    } else {
        kprint_at("[FAIL]", 35, current_row++);
    }

    kprint_at("Initializing VGA driver...........", 0, current_row);
    if (init_vga() == 0) {
        kprint_at("[ OK ]", 35, current_row++);
    } else {
        kprint_at("[FAIL]", 35, current_row++);
    }

    kprint_at("Loading keyboard driver...........", 0, current_row);
    if (init_keyboard() == 0) {
        kprint_at("[ OK ]", 35, current_row++);
    } else {
        kprint_at("[FAIL]", 35, current_row++);
    }

    kprint_at("Probing I/O Ports.................", 0, current_row);
    if (probe_io_ports() == 0) {
        kprint_at("[ OK ]", 35, current_row++);
    } else {
        kprint_at("[FAIL]", 35, current_row++);
    }

    kprint_at("Searching for PCI Devices.........", 0, current_row);
    if (search_pci() == 0) {
        kprint_at("[ OK ]", 35, current_row++);
    } else {
        kprint_at("[WARN]", 35, current_row++);
    }

    kprint_at("Initializing Scheduler............", 0, current_row);
    if (init_scheduler() == 0) {
        kprint_at("[ OK ]", 35, current_row++);
    } else {
        kprint_at("[FAIL]", 35, current_row++);
    }

    cursor_y = current_row;
    cursor_x = 0;
    kprint("\n");
    
    kprint_at("Initializing Disk Controller......", 0, current_row);
    extern void ata_init();
    ata_init();
    kprint_at("[ OK ]", 35, current_row++);

    kprint_at("Initializing FAT16 File System....", 0, current_row);
    cursor_y = current_row + 2;
    cursor_x = 0;
    kprint("\n");
    
    fat16_init(); 
    
    if (vfs_root != 0) {
        kprint_at("[ OK ]", 35, current_row);
        kprint("\n");
        current_row += 2;
    } else {
        kprint_at("[FAIL]", 35, current_row);
        kprint("\nERROR: Failed to initialize FAT16!\n");
        kprint("System halted.\n");
        while(1);
    }

    cursor_y = current_row + 1;
    cursor_x = 0;
    kprint("\nLuxOS Terminal is ready.\n> ");

    __asm__ __volatile__("sti");

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
                        video_memory[offset + 1] = WHITE_ON_BLACK;
                        update_hardware_cursor(cursor_x, cursor_y);
                    }
                }
            } 
            else if (buffer_idx < PAGE_SIZE - 1) { 
                key_buffer[buffer_idx++] = key;
                char temp_str[2] = {key, 0};
                kprint(temp_str);
            }
        }
        __asm__ __volatile__("hlt");
    }
}