PREFIX := riscv64-elf-

CC         := $(PREFIX)gcc
GDB        := $(PREFIX)gdb
OBJDUMP    := $(PREFIX)objdump
SIZE       := $(PREFIX)size
CLANG_TIDY := clang-tidy

QEMU := qemu-system-riscv64

BUILD_DIR := kernel/build
KERNEL    := $(BUILD_DIR)/kernel.elf
MAP       := $(BUILD_DIR)/kernel.map

LINKER_SCRIPT := kernel/linker/linker.ld

SOURCE_DIRS := kernel/arch/riscv64 kernel/core kernel/drivers
INCLUDES    := -Ikernel/include -Ikernel/arch/riscv64

C_SOURCES := $(shell find $(SOURCE_DIRS) -name '*.c')
S_SOURCES := $(shell find $(SOURCE_DIRS) -name '*.S')

OBJECTS := \
	$(C_SOURCES:%.c=$(BUILD_DIR)/%.o) \
	$(S_SOURCES:%.S=$(BUILD_DIR)/%.o)

DEPS := $(OBJECTS:.o=.d)

ARCH_FLAGS := \
	-march=rv64imab_zicsr \
	-mabi=lp64 \
	-mcmodel=medany

COMMON_FLAGS := \
	$(ARCH_FLAGS) \
	-ffreestanding \
	-fno-builtin \
	-fno-pie \
	-fno-stack-protector \
	-msmall-data-limit=0 \
	-ffunction-sections \
	-fdata-sections \
	-MMD \
	-MP \
	-g3

CFLAGS := \
	$(COMMON_FLAGS) \
	-Wall \
	-Wextra \
	-Werror \
	-O2 \
	-std=c11

ASFLAGS := $(COMMON_FLAGS)

LDFLAGS := \
	$(ARCH_FLAGS) \
	-nostdlib \
	-no-pie \
	-T $(LINKER_SCRIPT) \
	-Wl,--gc-sections \
	-Wl,-Map=$(MAP)

QEMU_FLAGS := \
	-machine virt \
	-smp 1 \
	-m 128M \
	-nographic \
	-bios default \
	-kernel $(KERNEL)


.PHONY: all
all: $(KERNEL)


$(KERNEL): $(OBJECTS) $(LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) $(OBJECTS) -o $@
	$(SIZE) $@


$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(INCLUDES) $(CFLAGS) -c $< -o $@


$(BUILD_DIR)/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(INCLUDES) $(ASFLAGS) -c $< -o $@


-include $(DEPS)


.PHONY: run
run: $(KERNEL)
	$(QEMU) $(QEMU_FLAGS)


.PHONY: debug
debug: $(KERNEL)
	$(QEMU) $(QEMU_FLAGS) -S -gdb tcp::1234


.PHONY: gdb
gdb: $(KERNEL)
	$(GDB) \
		-ex "target remote localhost:1234" \
		-ex "break kernel_main" \
		$(KERNEL)


.PHONY: disassemble
disassemble: $(KERNEL)
	$(OBJDUMP) -d -S $(KERNEL) > $(BUILD_DIR)/kernel.asm


.PHONY: tidy
tidy:
	@set -e; \
	for source in $(C_SOURCES); do \
		echo "TIDY $$source"; \
		$(CLANG_TIDY) "$$source" -- \
			--target=riscv64-unknown-elf \
			-march=rv64imab_zicsr \
			-mabi=lp64 \
			-ffreestanding \
			-std=c11 \
			$(INCLUDES); \
	done


.PHONY: clean
clean:
	rm -rf $(BUILD_DIR)

