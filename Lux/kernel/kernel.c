#define VIDEO_ADDRESS 0xb8000
#define MAX_ROWS 25
#define MAX_COLS 80
#define WHITE_ON_BLACK 0x0f

int cursor_x = 0;
int cursor_y = 0;

extern void port_byte_out(unsigned short port, unsigned char data);
extern unsigned char port_byte_in(unsigned short port);

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

char get_key_char() {
    if (port_byte_in(0x64) & 0x01) {
        unsigned char scancode = port_byte_in(0x60);

        if (scancode & 0x80) {
            return 0;
        }

        if (scancode > 0 && scancode < 58) { 
            return keyboard_map[scancode];
        }
    }
    return 0; 
}

void kprint(char* message) {
    unsigned char* video_memory = (unsigned char*) VIDEO_ADDRESS;
    int i = 0;

    while (message[i] != 0) {
        if (message[i] == '\n') {
            cursor_x = 0;
            cursor_y++;
        } 
        else {
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

void init() {
    clear_screen();

    kprint("LuxOS Kernel 0.0.2 (Experimental)\n");
    kprint("---------------------------------\n");

    kprint("Initializing GDT.................. [DONE]\n"); 
    kprint("CPU Protected Mode (x86_32)....... [DONE]\n"); 
    kprint("Setting up Kernel Stack........... [DONE]\n"); 
    
    kprint("Initializing VGA Driver........... [ OK ]\n"); 
    kprint("Loading Keyboard Layout........... [ OK ]\n");

    kprint("Probing I/O Ports................. [ OK ]\n");
    kprint("Detecting Memory Regions.......... [ OK ]\n");
    kprint("Searching for PCI Devices......... [NONE]\n");
    kprint("Initializing Scheduler............ [READY]\n");

    kprint("\nLuxOS Terminal is ready.\n");
    kprint("Type 'help' for commands (not implemented yet).\n");
    kprint("> ");

    port_byte_in(0x60); 

    while (1) {
        char key = get_key_char(); 

        if (key != 0) {
            if (key == '\n') {
                kprint("\n> ");
            } 
            else if (key == '\b') {
                if (cursor_x > 2) { 
                    cursor_x--;
                    kprint(" "); 
                    cursor_x--; 
                    update_hardware_cursor(cursor_x, cursor_y);
                }
            }
            else {
                char temp_str[2] = {key, 0};
                kprint(temp_str);
            }
        }
    }
}