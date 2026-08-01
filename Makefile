PREFIX ?= riscv64-elf-

# External tool names remain overrideable for distributions which install the
# same bare-metal toolchain under a different prefix.
CC         := $(PREFIX)gcc
GDB        := $(PREFIX)gdb
OBJDUMP    := $(PREFIX)objdump
OBJCOPY    := $(PREFIX)objcopy
SIZE       := $(PREFIX)size
CLANG_TIDY := clang-tidy
PYTHON     ?= python3

QEMU ?= qemu-system-riscv64

CLANG_TIDY_CHECKS := \
	-checks=-*,clang-analyzer-*,bugprone-*,-clang-analyzer-optin.performance.Padding,-bugprone-easily-swappable-parameters \
	--warnings-as-errors=*

BUILD_DIR := kernel/build
KERNEL    := $(BUILD_DIR)/kernel.elf
MAP       := $(BUILD_DIR)/kernel.map
ROOT_IMAGE := $(BUILD_DIR)/root.ext2

LINKER_SCRIPT := kernel/linker/linker.ld
USER_LINKER_SCRIPT := user/linker.ld

SOURCE_DIRS := kernel/arch/riscv64 kernel/core kernel/drivers kernel/memory
INCLUDES    := -Ikernel/include -Ikernel/arch/riscv64
USER_INCLUDES := -Iuser/include -Ikernel/include

C_SOURCES := $(shell find $(SOURCE_DIRS) -name '*.c')
S_SOURCES := $(shell find $(SOURCE_DIRS) -name '*.S')
USER_C_SOURCES := $(shell find user -name '*.c')
USER_S_SOURCES := $(shell find user -name '*.S')

OBJECTS := \
	$(C_SOURCES:%.c=$(BUILD_DIR)/%.o) \
	$(S_SOURCES:%.S=$(BUILD_DIR)/%.o)

USER_COMMON_OBJECTS := \
	$(BUILD_DIR)/user/start.o \
	$(BUILD_DIR)/user/syscall.o

# Each userspace path is a distinct ELF. All variants use the same small source,
# with a build-time selector allowing dead-code elimination to retain only the
# requested demonstration. The disk receives every image; the diagnostic
# ramfs retains only the original seven programs.
USER_PROGRAMS := hello fault process_a process_b syscall_test cat console_read init fs_test
USER_PROGRAM_hello := 0
USER_PROGRAM_fault := 1
USER_PROGRAM_process_a := 2
USER_PROGRAM_process_b := 3
USER_PROGRAM_syscall_test := 4
USER_PROGRAM_cat := 5
USER_PROGRAM_console_read := 6
USER_PROGRAM_init := 7
USER_PROGRAM_fs_test := 8
USER_ELFS := $(foreach program,$(USER_PROGRAMS),$(BUILD_DIR)/user/$(program)/program.elf)
USER_LOAD_ELFS := $(USER_ELFS:.elf=.load.elf)
USER_FALLBACK_PROGRAMS := hello fault process_a process_b syscall_test cat console_read
USER_FALLBACK_ELFS := $(foreach program,$(USER_FALLBACK_PROGRAMS),$(BUILD_DIR)/user/$(program)/program.load.elf)
USER_IMAGE_OBJECT := $(BUILD_DIR)/kernel/arch/riscv64/user_image.o

# Compiler-generated dependency files keep incremental header rebuilds correct.
DEPS := \
	$(OBJECTS:.o=.d) \
	$(USER_COMMON_OBJECTS:.o=.d) \
	$(foreach program,$(USER_PROGRAMS),$(BUILD_DIR)/user/$(program)/main.d)

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

USER_CFLAGS := \
	$(CFLAGS) \
	-fno-asynchronous-unwind-tables \
	-fno-unwind-tables

USER_ASFLAGS := $(COMMON_FLAGS)

LDFLAGS := \
	$(ARCH_FLAGS) \
	-nostdlib \
	-no-pie \
	-T $(LINKER_SCRIPT) \
	-Wl,--gc-sections \
	-Wl,-Map=$(MAP)

USER_LDFLAGS := \
	$(ARCH_FLAGS) \
	-nostdlib \
	-no-pie \
	-T $(USER_LINKER_SCRIPT) \
	-Wl,--gc-sections \
	-Wl,--build-id=none

QEMU_FLAGS := \
	-machine virt \
	-smp 1 \
	-m 128M \
	-nographic \
	-bios default \
	-kernel $(KERNEL) \
	-drive file=$(ROOT_IMAGE),format=raw,if=none,id=rose-root \
	-device virtio-blk-device,drive=rose-root \
	-global virtio-mmio.force-legacy=false


.PHONY: all
all: $(KERNEL) $(ROOT_IMAGE)


$(ROOT_IMAGE): tools/mkrosefs.py $(USER_LOAD_ELFS)
	@mkdir -p $(dir $@)
	$(PYTHON) tools/mkrosefs.py $@ \
		--file /bin/hello=$(BUILD_DIR)/user/hello/program.load.elf \
		--file /bin/fault=$(BUILD_DIR)/user/fault/program.load.elf \
		--file /bin/process-a=$(BUILD_DIR)/user/process_a/program.load.elf \
		--file /bin/process-b=$(BUILD_DIR)/user/process_b/program.load.elf \
		--file /bin/syscall-test=$(BUILD_DIR)/user/syscall_test/program.load.elf \
		--file /bin/cat=$(BUILD_DIR)/user/cat/program.load.elf \
		--file /bin/console-read=$(BUILD_DIR)/user/console_read/program.load.elf \
		--file /bin/fs-test=$(BUILD_DIR)/user/fs_test/program.load.elf \
		--file /sbin/init=$(BUILD_DIR)/user/init/program.load.elf


$(KERNEL): $(OBJECTS) $(LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) $(OBJECTS) -o $@
	$(SIZE) $@


define USER_PROGRAM_template
$(BUILD_DIR)/user/$(1)/main.o: user/main.c
	@mkdir -p $$(dir $$@)
	$$(CC) $$(USER_INCLUDES) $$(USER_CFLAGS) \
		-DROSE_PROGRAM=$$(USER_PROGRAM_$(1)) -c $$< -o $$@

$(BUILD_DIR)/user/$(1)/program.elf: \
		$(BUILD_DIR)/user/$(1)/main.o $$(USER_COMMON_OBJECTS) \
		$$(USER_LINKER_SCRIPT)
	@mkdir -p $$(dir $$@)
	$$(CC) $$(USER_LDFLAGS) \
		-Wl,-Map=$(BUILD_DIR)/user/$(1)/program.map \
		$(BUILD_DIR)/user/$(1)/main.o $$(USER_COMMON_OBJECTS) -o $$@
	$$(SIZE) $$@
endef

$(foreach program,$(USER_PROGRAMS),$(eval $(call USER_PROGRAM_template,$(program))))


# Strip symbol/debug data before filesystem installation, but retain each ELF
# and its program headers so the kernel exercises a real loader, not flat blobs.
$(BUILD_DIR)/user/%/program.load.elf: $(BUILD_DIR)/user/%/program.elf
	$(OBJCOPY) --strip-all $< $@


# user_image.S uses .incbin, so this explicit prerequisite rebuilds its object
# whenever the independently linked user executable changes.
$(USER_IMAGE_OBJECT): $(USER_FALLBACK_ELFS)


$(BUILD_DIR)/user/%.o: user/%.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_INCLUDES) $(USER_CFLAGS) -c $< -o $@


$(BUILD_DIR)/user/%.o: user/%.S
	@mkdir -p $(dir $@)
	$(CC) $(USER_INCLUDES) $(USER_ASFLAGS) -c $< -o $@


$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(INCLUDES) $(CFLAGS) -c $< -o $@


$(BUILD_DIR)/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(INCLUDES) $(ASFLAGS) -c $< -o $@


-include $(DEPS)


.PHONY: run
run: $(KERNEL) $(ROOT_IMAGE)
	$(QEMU) $(QEMU_FLAGS)


.PHONY: debug
debug: $(KERNEL) $(ROOT_IMAGE)
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


# Analyze kernel and user C separately because they use different headers.
.PHONY: tidy
tidy:
	@set -e; \
	for source in $(C_SOURCES); do \
		echo "TIDY $$source"; \
		$(CLANG_TIDY) $(CLANG_TIDY_CHECKS) "$$source" -- \
			--target=riscv64-unknown-elf \
			-march=rv64imab_zicsr \
			-mabi=lp64 \
			-ffreestanding \
			-std=c11 \
			$(INCLUDES); \
	done
	@set -e; \
	for source in $(USER_C_SOURCES); do \
		echo "TIDY $$source"; \
		$(CLANG_TIDY) $(CLANG_TIDY_CHECKS) "$$source" -- \
			--target=riscv64-unknown-elf \
			-march=rv64imab_zicsr \
			-mabi=lp64 \
			-ffreestanding \
			-std=c11 \
			$(USER_INCLUDES); \
	done


.PHONY: test
test: $(KERNEL) $(ROOT_IMAGE)
	QEMU=$(QEMU) KERNEL=$(KERNEL) ROOT_IMAGE=$(ROOT_IMAGE) $(PYTHON) tests/qemu_smoke.py


# A second RAM size proves that memory bounds come from the DTB.
.PHONY: test-platform
test-platform: $(KERNEL) $(ROOT_IMAGE)
	QEMU=$(QEMU) KERNEL=$(KERNEL) ROOT_IMAGE=$(ROOT_IMAGE) QEMU_MEMORY=192M \
		$(PYTHON) tests/qemu_smoke.py


.PHONY: check
check:
	$(MAKE) tidy
	$(MAKE) test
	$(MAKE) test-platform


.PHONY: clean
clean:
	rm -rf $(BUILD_DIR)
