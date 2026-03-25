CC = gcc
AS = gcc
AR = ar
CFLAGS = -m32 -ffreestanding -fno-pie -fno-stack-protector -nostdlib -Iinclude -Wall -Wextra
ASFLAGS = -m32 -ffreestanding -fno-pie -nostdlib

SRC_DIR = src
BUILD_DIR = build
INCLUDE_DIR = include

C_SRCS = $(wildcard $(SRC_DIR)/*.c)
S_SRCS = $(wildcard $(SRC_DIR)/*.S)
C_OBJS = $(patsubst $(SRC_DIR)/%.c, $(BUILD_DIR)/%.o, $(C_SRCS))
S_OBJS = $(patsubst $(SRC_DIR)/%.S, $(BUILD_DIR)/%.o, $(S_SRCS))
OBJS = $(C_OBJS) $(S_OBJS)

TARGET = libc.a

all: $(TARGET)

$(TARGET): $(OBJS)
	$(AR) rcs $@ $^

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.S
	@mkdir -p $(BUILD_DIR)
	$(AS) $(ASFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD_DIR) $(TARGET)

.PHONY: all clean