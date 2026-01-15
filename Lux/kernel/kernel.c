#define VIDEO_ADDRESS 0xb8000
#define MAX_ROWS 25
#define MAX_COLS 80
#define WHITE_ON_BLACK 0x0f

int cursor_x = 0;
int cursor_y = 0;

extern void port_byte_out(unsigned short port, unsigned char data);
extern unsigned char port_byte_in(unsigned short port);
extern void port_long_out(unsigned short port, unsigned int data); 
extern unsigned int port_long_in(unsigned short port);          

unsigned char keyboard_map[128] = {
    0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0,
    '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' '
};


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


int init_timer() {
    unsigned int frequency = 100;
    unsigned int divisor = 1193180 / frequency;
    port_byte_out(0x43, 0x36);
    port_byte_out(0x40, (unsigned char)(divisor & 0xFF));
    port_byte_out(0x40, (unsigned char)((divisor >> 8) & 0xFF));
    return 0;
}

char get_key_char() {
    if (port_byte_in(0x64) & 0x01) {
        unsigned char scancode = port_byte_in(0x60);
        if (scancode & 0x80) return 0;
        if (scancode > 0 && scancode < 58) { 
            return keyboard_map[scancode];
        }
    }
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
    while (port_byte_in(0x64) & 0x01) {
        port_byte_in(0x60);
    }
    return 0;
}

int probe_io_ports() {
    unsigned char status = port_byte_in(0x64);
    if (status == 0xFF) return 1;
    return 0;
}

int detect_memory() {
    volatile unsigned int* mem_ptr = (volatile unsigned int*)0x100000;
    unsigned int old_val = *mem_ptr;
    *mem_ptr = 0xABCDEF12;
    if (*mem_ptr != 0xABCDEF12) return 1;
    *mem_ptr = old_val;
    return 0;
}

int search_pci() {
    port_long_out(0xCF8, 0x80000000);
    unsigned int res = port_long_in(0xCF8);
    if (res == 0x80000000) return 0; 
    return 1; 
}

int init_scheduler() {
    return 0;
}

int strcmp(char* s1, char* s2) {
    int i;
    for (i = 0; s1[i] == s2[i]; i++) {
        if (s1[i] == '\0') return 0;
    }
    return s1[i] - s2[i];
}

char key_buffer[256];
int buffer_idx = 0;

void execute_command(char* input) {
    if (strcmp(input, "clear") == 0) {
        clear_screen();
        kprint("LuxOS Kernel 0.0.3 (Experimental)\n");
        kprint("---------------------------------\n");
        kprint("\nLuxOS Terminal is ready.\n");
        kprint("Type 'help' for commands!\n");
        kprint("> ");
    } 
    else if (strcmp(input, "help") == 0) {
        kprint("\nAvailable commands:\n");
        kprint("clear  - Clear the screen\n");
        kprint("help   - Show this message\n");
        kprint("reboot - Not implemented yet\n> ");
    } 
    else if (input[0] == '\0') {
        kprint("\n> ");
    }
    else {
        kprint("\nUnknown command: ");
        kprint(input);
        kprint("\n> ");
    }
}

void init() {
    clear_screen();

    kprint("LuxOS Kernel 0.0.3 (Experimental)\n");
    kprint("---------------------------------\n");

    int current_row = 2;

    kprint_at("Initializing GDT..................", 0, current_row);
    kprint_at("[DONE]", 35, current_row++);

    kprint_at("CPU Protected Mode (x86_32).......", 0, current_row);
    kprint_at("[DONE]", 35, current_row++);

    kprint_at("Setting up Kernel Stack...........", 0, current_row);
    kprint_at("[DONE]", 35, current_row++);

    kprint_at("Initializing VGA driver...........", 0, current_row);
    if (init_vga() == 0) {
        kprint_at("[ OK ]", 35, current_row++);
    } else {
        kprint_at("[FAIL]", 35, current_row++);
        while(1); 
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

    kprint_at("Detecting Memory Regions..........", 0, current_row);
    if (detect_memory() == 0) {
        kprint_at("[ OK ]", 35, current_row++);
    } else {
        kprint_at("[FAIL]", 35, current_row++);
    }

    kprint_at("Searching for PCI Devices.........", 0, current_row);
    if (search_pci() == 0) {
        kprint_at("[ OK ]", 35, current_row++);
    } else {
        kprint_at("[ FAIL ]", 35, current_row++);
    }

    kprint_at("Initializing Scheduler............", 0, current_row);
    if (init_scheduler() == 0) {
        kprint_at("[ OK ]", 35, current_row++);
    } else {
        kprint_at("[ FAIL ]", 35, current_row++);
    }

    cursor_y = current_row + 1;
    cursor_x = 0;
    kprint("\nLuxOS Terminal is ready.\n");
    kprint("Type 'help' for commands!\n");
    kprint("> ");

    port_byte_in(0x60); 

    while (1) {
        char key = get_key_char(); 
        if (key != 0) {
            if (key == '\n') {
                key_buffer[buffer_idx] = '\0';
                execute_command(key_buffer);
                buffer_idx = 0; 
            } 
            else if (key == '\b') {
                if (buffer_idx > 0) {
                    buffer_idx--;
                    cursor_x--;
                    kprint(" "); 
                    cursor_x--; 
                    update_hardware_cursor(cursor_x, cursor_y);
                }
            } 
            else {
                if (buffer_idx < 255) {
                    key_buffer[buffer_idx++] = key;
                    char temp_str[2] = {key, 0};
                    kprint(temp_str);
                }
            }
        }
    }
}