PREFIX ?= riscv64-elf-

.DEFAULT_GOAL := all
.DELETE_ON_ERROR:

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

# This is intentionally not caller-overridable: `clean` must always have one
# narrow, repository-local target.
override BUILD_DIR := kernel/build
KERNEL    := $(BUILD_DIR)/kernel.elf
MAP       := $(BUILD_DIR)/kernel.map
ROOT_IMAGE := $(BUILD_DIR)/root.ext2
RUN_ROOT_IMAGE := $(BUILD_DIR)/run-root.ext2

LINKER_SCRIPT := kernel/linker/linker.ld
USER_LINKER_SCRIPT := user/linker.ld

SOURCE_DIRS := kernel/arch/riscv64 kernel/core kernel/drivers kernel/memory
INCLUDES    := -Ikernel/include -Ikernel/arch/riscv64
USER_INCLUDES := -Iuser/include -Ikernel/include

C_SOURCES := $(sort $(shell find $(SOURCE_DIRS) -name '*.c'))
S_SOURCES := $(sort $(shell find $(SOURCE_DIRS) -name '*.S'))
USER_C_SOURCES := $(sort $(shell find user -name '*.c'))
USER_S_SOURCES := $(sort $(shell find user -name '*.S'))

OBJECTS := \
	$(C_SOURCES:%.c=$(BUILD_DIR)/%.o) \
	$(S_SOURCES:%.S=$(BUILD_DIR)/%.o)

USER_COMMON_OBJECTS := \
	$(BUILD_DIR)/user/start.o \
	$(BUILD_DIR)/user/syscall.o

# Each userspace path is a distinct ELF. Demonstrations share a build-time
# selected source, while /bin/sh adds its own implementation object. The disk
# receives every image; the diagnostic ramfs retains its programs and shell.
USER_PROGRAMS := hello fault process_a process_b syscall_test cat console_read init fs_test args_env execve execve_target pipe_test pipe_writer sh ls echo pwd env mkdir rm descriptor_test signal_exec_test desktop gui_terminal gui_files gui_monitor
# Use the ABI enum names directly so adding or reordering an enum cannot
# silently build a program with the wrong implementation.
USER_PROGRAM_hello := USER_PROGRAM_HELLO
USER_PROGRAM_fault := USER_PROGRAM_FAULT
USER_PROGRAM_process_a := USER_PROGRAM_MULTI_A
USER_PROGRAM_process_b := USER_PROGRAM_MULTI_B
USER_PROGRAM_syscall_test := USER_PROGRAM_SYSCALL_TEST
USER_PROGRAM_cat := USER_PROGRAM_CAT
USER_PROGRAM_console_read := USER_PROGRAM_CONSOLE_READ
USER_PROGRAM_init := USER_PROGRAM_INIT
USER_PROGRAM_fs_test := USER_PROGRAM_FS_TEST
USER_PROGRAM_args_env := USER_PROGRAM_ARGUMENTS_ENVIRONMENT
USER_PROGRAM_execve := USER_PROGRAM_EXECVE
USER_PROGRAM_execve_target := USER_PROGRAM_EXECVE_TARGET
USER_PROGRAM_pipe_test := USER_PROGRAM_PIPE_TEST
USER_PROGRAM_pipe_writer := USER_PROGRAM_PIPE_WRITER
USER_PROGRAM_sh := USER_PROGRAM_SH
USER_PROGRAM_ls := USER_PROGRAM_LS
USER_PROGRAM_echo := USER_PROGRAM_ECHO
USER_PROGRAM_pwd := USER_PROGRAM_PWD
USER_PROGRAM_env := USER_PROGRAM_ENV
USER_PROGRAM_mkdir := USER_PROGRAM_MKDIR
USER_PROGRAM_rm := USER_PROGRAM_RM
USER_PROGRAM_descriptor_test := USER_PROGRAM_DESCRIPTOR_TEST
USER_PROGRAM_signal_exec_test := USER_PROGRAM_SIGNAL_EXEC_TEST
USER_PROGRAM_desktop := USER_PROGRAM_DESKTOP
USER_PROGRAM_gui_terminal := USER_PROGRAM_GUI_TERMINAL
USER_PROGRAM_gui_files := USER_PROGRAM_GUI_FILES
USER_PROGRAM_gui_monitor := USER_PROGRAM_GUI_SYSTEM_MONITOR
USER_ELFS := $(foreach program,$(USER_PROGRAMS),$(BUILD_DIR)/user/$(program)/program.elf)
USER_LOAD_ELFS := $(USER_ELFS:.elf=.load.elf)
USER_FALLBACK_PROGRAMS := hello fault process_a process_b syscall_test cat console_read sh ls echo pwd env mkdir rm descriptor_test signal_exec_test
USER_FALLBACK_ELFS := $(foreach program,$(USER_FALLBACK_PROGRAMS),$(BUILD_DIR)/user/$(program)/program.load.elf)
USER_IMAGE_OBJECT := $(BUILD_DIR)/kernel/arch/riscv64/user_image.o

USER_EXTRA_OBJECTS_sh := $(BUILD_DIR)/user/sh.o
USER_EXTRA_OBJECTS_desktop := $(BUILD_DIR)/user/desktop.o
USER_EXTRA_OBJECTS_gui_terminal := $(BUILD_DIR)/user/gui_terminal.o $(BUILD_DIR)/user/gui.o
USER_EXTRA_OBJECTS_gui_files := $(BUILD_DIR)/user/gui_files.o $(BUILD_DIR)/user/gui.o
USER_EXTRA_OBJECTS_gui_monitor := $(BUILD_DIR)/user/gui_monitor.o $(BUILD_DIR)/user/gui.o

# Compiler-generated dependency files keep incremental header rebuilds correct.
DEPS := \
	$(OBJECTS:.o=.d) \
	$(USER_COMMON_OBJECTS:.o=.d) \
	$(BUILD_DIR)/user/sh.d \
	$(BUILD_DIR)/user/desktop.d \
	$(BUILD_DIR)/user/gui.d \
	$(BUILD_DIR)/user/gui_terminal.d \
	$(BUILD_DIR)/user/gui_files.d \
	$(BUILD_DIR)/user/gui_monitor.d \
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

# Compact file padding does not change the page-separated virtual RX/RW layout.
USER_LDFLAGS := \
	$(ARCH_FLAGS) \
	-nostdlib \
	-no-pie \
	-T $(USER_LINKER_SCRIPT) \
	-Wl,-z,max-page-size=0x100 \
	-Wl,--gc-sections \
	-Wl,--build-id=none

QEMU_FLAGS := \
	-machine virt \
	-smp 1 \
	-m 128M \
	-nographic \
	-bios default \
	-kernel $(KERNEL) \
	-drive file=$(RUN_ROOT_IMAGE),format=raw,if=none,id=rose-root \
	-device virtio-blk-device,drive=rose-root \
	-global virtio-mmio.force-legacy=false

QEMU_GUI_FLAGS := \
	-machine virt \
	-smp 1 \
	-m 128M \
	-bios default \
	-kernel $(KERNEL) \
	-drive file=$(RUN_ROOT_IMAGE),format=raw,if=none,id=rose-root \
	-device virtio-blk-device,drive=rose-root \
	-device virtio-gpu-device \
	-device virtio-keyboard-device \
	-device virtio-tablet-device \
	-global virtio-mmio.force-legacy=false \
	-serial stdio \
	-monitor none


.PHONY: all
all: $(KERNEL) $(ROOT_IMAGE)


$(ROOT_IMAGE): tools/mkrosefs.py $(USER_LOAD_ELFS) Makefile
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
		--file /bin/args-env=$(BUILD_DIR)/user/args_env/program.load.elf \
		--file /bin/execve=$(BUILD_DIR)/user/execve/program.load.elf \
		--file /bin/execve-target=$(BUILD_DIR)/user/execve_target/program.load.elf \
		--file /bin/pipe-test=$(BUILD_DIR)/user/pipe_test/program.load.elf \
		--file /bin/pipe-writer=$(BUILD_DIR)/user/pipe_writer/program.load.elf \
		--file /bin/sh=$(BUILD_DIR)/user/sh/program.load.elf \
		--file /bin/ls=$(BUILD_DIR)/user/ls/program.load.elf \
		--file /bin/echo=$(BUILD_DIR)/user/echo/program.load.elf \
		--file /bin/pwd=$(BUILD_DIR)/user/pwd/program.load.elf \
		--file /bin/env=$(BUILD_DIR)/user/env/program.load.elf \
		--file /bin/mkdir=$(BUILD_DIR)/user/mkdir/program.load.elf \
		--file /bin/rm=$(BUILD_DIR)/user/rm/program.load.elf \
		--file /bin/descriptor-test=$(BUILD_DIR)/user/descriptor_test/program.load.elf \
		--file /bin/signal-exec-test=$(BUILD_DIR)/user/signal_exec_test/program.load.elf \
		--file /bin/desktop=$(BUILD_DIR)/user/desktop/program.load.elf \
		--file /bin/gui-terminal=$(BUILD_DIR)/user/gui_terminal/program.load.elf \
		--file /bin/gui-files=$(BUILD_DIR)/user/gui_files/program.load.elf \
		--file /bin/gui-monitor=$(BUILD_DIR)/user/gui_monitor/program.load.elf \
		--file /sbin/init=$(BUILD_DIR)/user/init/program.load.elf


# Interactive boots persist their filesystem changes without modifying the
# deterministic image consumed by tests and fresh runtime-disk creation.
$(RUN_ROOT_IMAGE): $(ROOT_IMAGE)
	cp $< $@


$(KERNEL): $(OBJECTS) $(LINKER_SCRIPT) Makefile
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) $(OBJECTS) -o $@
	$(SIZE) $@


define USER_PROGRAM_template
$(BUILD_DIR)/user/$(1)/main.o: user/main.c Makefile
	@mkdir -p $$(dir $$@)
	$$(CC) $$(USER_INCLUDES) $$(USER_CFLAGS) \
		-DROSE_PROGRAM=$$(USER_PROGRAM_$(1)) \
		$$(USER_PROGRAM_CFLAGS_$(1)) -c $$< -o $$@

$(BUILD_DIR)/user/$(1)/program.elf: \
		$(BUILD_DIR)/user/$(1)/main.o $$(USER_COMMON_OBJECTS) \
		$$(USER_EXTRA_OBJECTS_$(1)) \
		$$(USER_LINKER_SCRIPT) Makefile
	@mkdir -p $$(dir $$@)
	$$(CC) $$(USER_LDFLAGS) \
		-Wl,-Map=$(BUILD_DIR)/user/$(1)/program.map \
		$(BUILD_DIR)/user/$(1)/main.o $$(USER_COMMON_OBJECTS) \
		$$(USER_EXTRA_OBJECTS_$(1)) -o $$@
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


$(BUILD_DIR)/user/%.o: user/%.c Makefile
	@mkdir -p $(dir $@)
	$(CC) $(USER_INCLUDES) $(USER_CFLAGS) -c $< -o $@


$(BUILD_DIR)/user/sh.o: user/sh.c Makefile
	@mkdir -p $(dir $@)
	$(CC) $(USER_INCLUDES) $(USER_CFLAGS) $(USER_PROGRAM_CFLAGS_sh) \
		-c $< -o $@


$(BUILD_DIR)/user/%.o: user/%.S Makefile
	@mkdir -p $(dir $@)
	$(CC) $(USER_INCLUDES) $(USER_ASFLAGS) -c $< -o $@


$(BUILD_DIR)/%.o: %.c Makefile
	@mkdir -p $(dir $@)
	$(CC) $(INCLUDES) $(CFLAGS) -c $< -o $@


$(BUILD_DIR)/%.o: %.S Makefile
	@mkdir -p $(dir $@)
	$(CC) $(INCLUDES) $(ASFLAGS) -c $< -o $@


-include $(DEPS)


.PHONY: run
run: $(KERNEL) $(RUN_ROOT_IMAGE)
	$(QEMU) $(QEMU_FLAGS)


.PHONY: run-gui
run-gui: $(KERNEL) $(RUN_ROOT_IMAGE)
	$(QEMU) $(QEMU_GUI_FLAGS)


.PHONY: debug
debug: $(KERNEL) $(RUN_ROOT_IMAGE)
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
		$(CLANG_TIDY) "$$source" -- \
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
		$(CLANG_TIDY) "$$source" -- \
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


.PHONY: test-graphics
test-graphics: $(KERNEL) $(ROOT_IMAGE)
	QEMU=$(QEMU) KERNEL=$(KERNEL) ROOT_IMAGE=$(ROOT_IMAGE) \
		QEMU_GRAPHICS=1 QEMU_GRAPHICS_ONLY=1 $(PYTHON) tests/qemu_smoke.py


.PHONY: test-host
test-host:
	$(PYTHON) -m unittest discover -s tests -t . -p 'test_*.py'
	$(PYTHON) -m py_compile tests/qemu_smoke.py tools/mkrosefs.py


.PHONY: check
check:
	$(MAKE) test-host
	$(MAKE) tidy
	$(MAKE) test
	$(MAKE) test-platform
	$(MAKE) test-graphics


# Start from an empty build directory when validating a release or CI change.
.PHONY: verify
verify:
	$(MAKE) clean
	$(MAKE) all
	$(MAKE) check


.PHONY: clean
clean:
	rm -rf $(BUILD_DIR)
