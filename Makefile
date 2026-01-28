# ==============================================================================
# LuxOS Makefile 
# ==============================================================================

BOOT_DIR         = Lumen/boot
KERN_CORE_DIR    = Lux/kernel/core
KERN_SHELL_DIR   = Lux/kernel/shell
KERN_MEM_DIR     = Lux/kernel/memory
KERN_PROC_DIR    = Lux/kernel/proc
KERN_LIBC_DIR    = Lux/libc
DRIVER_INPUT_DIR = Lux/drivers/input
DRIVER_BLOCK_DIR = Lux/drivers/block
FS_VFS_DIR       = Lux/fs/vfs
FS_FAT16_DIR     = Lux/fs/fat16
BUILD_DIR        = build

CFLAGS = -m32 -ffreestanding -fno-pie -fno-stack-protector -nostdlib \
         -I$(KERN_CORE_DIR) \
         -I$(KERN_SHELL_DIR) \
         -I$(KERN_MEM_DIR) \
         -I$(KERN_PROC_DIR) \
         -I$(KERN_LIBC_DIR) \
         -I$(DRIVER_INPUT_DIR) \
         -I$(DRIVER_BLOCK_DIR) \
         -I$(FS_VFS_DIR) \
         -I$(FS_FAT16_DIR) \
         -Wall

LDFLAGS = -m elf_i386 -Ttext 0x7e00 --oformat binary

OBJ = $(BUILD_DIR)/kernel_entry.o \
      $(BUILD_DIR)/kernel.o \
      $(BUILD_DIR)/shell.o \
      $(BUILD_DIR)/libc.o \
      $(BUILD_DIR)/memory.o \
      $(BUILD_DIR)/task.o \
      $(BUILD_DIR)/vfs.o \
      $(BUILD_DIR)/fat16.o \
      $(BUILD_DIR)/ata.o \
      $(BUILD_DIR)/syscall.o \
      $(BUILD_DIR)/interrupt.o \
      $(BUILD_DIR)/io.o \
      $(BUILD_DIR)/mm.o

all: $(BUILD_DIR)/luxos.img
	@echo "--------------------------------------------------"
	@echo "Сборка LuxOS завершена успешно!"
	@echo "Ядро: $(BUILD_DIR)/kernel.bin"
	@echo "Образ: $(BUILD_DIR)/luxos.img"
	@echo "--------------------------------------------------"

$(BUILD_DIR)/luxos.img: $(BUILD_DIR)/boot.bin $(BUILD_DIR)/kernel.bin
	cat $^ > $@
	truncate -s 1440K $@

$(BUILD_DIR)/boot.bin: $(BOOT_DIR)/boot.asm
	@mkdir -p $(BUILD_DIR)
	nasm -f bin $< -o $@

$(BUILD_DIR)/kernel.bin: $(OBJ)
	ld $(LDFLAGS) -o $@ $^


$(BUILD_DIR)/kernel_entry.o: $(KERN_CORE_DIR)/kernel.asm
	@mkdir -p $(BUILD_DIR)
	nasm -f elf32 $< -o $@

$(BUILD_DIR)/interrupt.o: $(KERN_CORE_DIR)/interrupt.asm
	@mkdir -p $(BUILD_DIR)
	nasm -f elf32 $< -o $@

$(BUILD_DIR)/io.o: $(DRIVER_INPUT_DIR)/io.asm
	@mkdir -p $(BUILD_DIR)
	nasm -f elf32 $< -o $@

$(BUILD_DIR)/mm.o: $(KERN_MEM_DIR)/mm.asm
	@mkdir -p $(BUILD_DIR)
	nasm -f elf32 $< -o $@


$(BUILD_DIR)/kernel.o: $(KERN_CORE_DIR)/kernel.c
	@mkdir -p $(BUILD_DIR)
	gcc $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/shell.o: $(KERN_SHELL_DIR)/shell.c
	@mkdir -p $(BUILD_DIR)
	gcc $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/syscall.o: $(KERN_CORE_DIR)/syscall.c
	@mkdir -p $(BUILD_DIR)
	gcc $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/libc.o: $(KERN_LIBC_DIR)/libc.c
	@mkdir -p $(BUILD_DIR)
	gcc $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/memory.o: $(KERN_MEM_DIR)/memory.c
	@mkdir -p $(BUILD_DIR)
	gcc $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/task.o: $(KERN_PROC_DIR)/task.c
	@mkdir -p $(BUILD_DIR)
	gcc $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/vfs.o: $(FS_VFS_DIR)/vfs.c
	@mkdir -p $(BUILD_DIR)
	gcc $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/fat16.o: $(FS_FAT16_DIR)/fat16.c
	@mkdir -p $(BUILD_DIR)
	gcc $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/ata.o: $(DRIVER_BLOCK_DIR)/ata.c
	@mkdir -p $(BUILD_DIR)
	gcc $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD_DIR)

.PHONY: all clean