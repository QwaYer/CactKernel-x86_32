#ifndef KERNEL_H
#define KERNEL_H

#define VIDEO_ADDRESS 0xb8000
#define MAX_ROWS 25
#define MAX_COLS 80
#define WHITE_ON_BLACK 0x0f

#define PAGE_PRESENT 0x1
#define PAGE_RW      0x2
#define PAGE_USER    0x4

#define PD_INDEX(vaddr) ((vaddr >> 22) & 0x3FF)
#define PT_INDEX(vaddr) ((vaddr >> 12) & 0x3FF)

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
void hex_to_ascii(unsigned int n, char str[]);

void init_pic();
int init_idt();
void set_idt_gate(int n, unsigned int handler);
void keyboard_handler(); 

struct context_frame; 
void exception_handler(struct context_frame* regs);

static inline unsigned int read_cr2() {
    unsigned int val;
    __asm__ __volatile__("mov %%cr2, %0" : "=r"(val));
    return val;
}

int init_vga();
int init_keyboard();
int probe_io_ports();
int detect_memory();
int search_pci();
void init_timer(unsigned int frequency);

struct task_struct; 
int init_scheduler();
void task_init();
void schedule();
void list_tasks();
struct task_struct* create_task(void (*entry_point)());
extern void switch_to(unsigned int* old_esp, unsigned int new_esp);

void init_paging();
void switch_paging(unsigned int* pd);
void vmm_map(unsigned int virtual_addr, unsigned int physical_addr, int flags);
unsigned int* vmm_create_address_space();
unsigned int get_free_heap_memory();

extern void ata_init();
extern void ata_read_sector(unsigned int lba, unsigned char* buffer);
extern void ata_write_sector(unsigned int lba, unsigned char* buffer);
extern void fat16_init();
extern void fat16_list_root();

void kernel_setup_hardware();

#endif