#ifndef KERNEL_H
#define KERNEL_H

#define VIDEO_ADDRESS 0xb8000
#define MAX_ROWS 25
#define MAX_COLS 80
#define WHITE_ON_BLACK 0x0f

typedef void (*command_handler_t)(char* args);

void cmd_help(char* args);
void cmd_clear(char* args);
void cmd_cpu(char* args);
void cmd_ls(char* args);
void cmd_cat(char* args);
void cmd_touch(char* args);
void cmd_mem(char* args);
void cmd_uptime(char* args);
void cmd_reboot(char* args);

struct shell_command {
    char* name;
    char* description;
    command_handler_t handler;
};

extern struct shell_command commands[];
extern const int COMMAND_COUNT;

extern void port_byte_out(unsigned short port, unsigned char data);
extern unsigned char port_byte_in(unsigned short port);
extern void port_word_out(unsigned short port, unsigned short data);
extern unsigned short port_word_in(unsigned short port);
extern void port_long_out(unsigned short port, unsigned int data);
extern unsigned int port_long_in(unsigned short port);

extern void timer_isr();
extern void keyboard_isr();
extern void syscall_isr();
extern void isr0();
extern void isr13();
extern void isr14();
extern void isr_common_stub();

void kprint(char* message);
void kprint_at(char* message, int x, int y);
void clear_screen();
void scroll();
void update_hardware_cursor(int x, int y);
void boot_log(char* component, int status);

void itoa(int n, char str[]);
int atoi(char* str);
int strcmp(char* s1, char* s2);
int compare_string(char* s1, char* s2);

void init_pic();
int init_idt();
void set_idt_gate(int n, unsigned int handler);
void keyboard_handler(); 
void exception_handler();

int init_vga();
int init_keyboard();
int probe_io_ports();
int detect_memory();
int search_pci();
void init_timer(unsigned int frequency);

int init_scheduler();
struct task_struct; 
extern struct task_struct* create_task(void (*entry_point)());

extern void ata_init();
extern void ata_read_sector(unsigned int lba, unsigned char* buffer);
extern void ata_write_sector(unsigned int lba, unsigned char* buffer);

extern void fat16_init();
struct vfs_node; 
extern struct vfs_node* fat16_finddir(struct vfs_node* node, char* name);
extern void fat16_list_root();

void execute_command(char* input);
void kernel_setup_hardware();

#endif