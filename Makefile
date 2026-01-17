BOOT_DIR = Lumen/boot
KERN_DIR = Lux/kernel
BUILD_DIR = build

CFLAGS = -m32 -ffreestanding -fno-pie -fno-stack-protector -nostdlib -I$(KERN_DIR) -c


LDFLAGS = -m elf_i386 -Ttext 0x7e00 --oformat binary

OBJ = $(BUILD_DIR)/kernel_entry.o \
      $(BUILD_DIR)/io.o \
      $(BUILD_DIR)/interrupt.o \
      $(BUILD_DIR)/memory.o \
      $(BUILD_DIR)/vfs.o \
      $(BUILD_DIR)/ata.o \
      $(BUILD_DIR)/kernel.o

all: $(BUILD_DIR)/luxos.img

$(BUILD_DIR)/luxos.img: $(BUILD_DIR)/boot.bin $(BUILD_DIR)/kernel.bin
	@mkdir -p $(BUILD_DIR)
	cat $^ > $@
	truncate -s 1048576 $@

$(BUILD_DIR)/boot.bin: $(BOOT_DIR)/boot.asm
	@mkdir -p $(BUILD_DIR)
	nasm -f bin $< -o $@

$(BUILD_DIR)/kernel.bin: $(OBJ)
	ld $(LDFLAGS) -o $@ $^

$(BUILD_DIR)/kernel_entry.o: $(KERN_DIR)/kernel.asm
	@mkdir -p $(BUILD_DIR)
	nasm -f elf32 $< -o $@

$(BUILD_DIR)/io.o: $(KERN_DIR)/io.asm
	@mkdir -p $(BUILD_DIR)
	nasm -f elf32 $< -o $@

$(BUILD_DIR)/interrupt.o: $(KERN_DIR)/interrupt.asm
	@mkdir -p $(BUILD_DIR)
	nasm -f elf32 $< -o $@

$(BUILD_DIR)/%.o: $(KERN_DIR)/%.c
	@mkdir -p $(BUILD_DIR)
	gcc $(CFLAGS) $< -o $@

clean:
	rm -rf $(BUILD_DIR)