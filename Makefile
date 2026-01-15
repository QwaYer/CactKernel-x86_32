# Пути к папкам
BOOT_DIR = Lumen/boot
KERN_DIR = Lux/kernel
BUILD_DIR = build

# Правило по умолчанию
all: $(BUILD_DIR)/luxos.img

$(BUILD_DIR)/luxos.img: $(BUILD_DIR)/boot.bin $(BUILD_DIR)/kernel.bin
	cat $^ > $@
	truncate -s 10240 $@

$(BUILD_DIR)/boot.bin: $(BOOT_DIR)/boot.asm
	nasm -f bin $< -o $@

$(BUILD_DIR)/kernel.bin: $(BUILD_DIR)/kernel_entry.o $(BUILD_DIR)/kernel.o
	ld -m elf_i386 -o $@ -Ttext 0x7e00 $^ --oformat binary

$(BUILD_DIR)/kernel_entry.o: $(KERN_DIR)/kernel.asm
	nasm -f elf32 $< -o $@

$(BUILD_DIR)/kernel.o: $(KERN_DIR)/kernel.c
	gcc -m32 -ffreestanding -fno-pie -c $< -o $@

clean:
	rm -rf $(BUILD_DIR)/*.bin $(BUILD_DIR)/*.o $(BUILD_DIR)/*.img