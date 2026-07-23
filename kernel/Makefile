# ==============================================================================
# Kernel build configuration
# ==============================================================================

TOOLCHAIN_PREFIX ?= riscv64-elf-

CC      := $(TOOLCHAIN_PREFIX)gcc
OBJDUMP := $(TOOLCHAIN_PREFIX)objdump
READELF := $(TOOLCHAIN_PREFIX)readelf
NM      := $(TOOLCHAIN_PREFIX)nm
SIZE    := $(TOOLCHAIN_PREFIX)size
GDB     := $(TOOLCHAIN_PREFIX)gdb

QEMU ?= qemu-system-riscv64

CLANG_FORMAT ?= clang-format
CLANG_TIDY   ?= clang-tidy


# ==============================================================================
# Target configuration
# ==============================================================================

ARCH     ?= riscv64
PLATFORM ?= qemu_virt

RISCV_ARCH ?= rv64imab_zicsr
RISCV_ABI  ?= lp64
CODE_MODEL ?= medany

HARTS    ?= 1
RAM_SIZE ?= 128M

TARGET        := kernel
BUILD_DIR     := build
LINKER_SCRIPT := linker/linker.ld

ELF         := $(BUILD_DIR)/$(TARGET).elf
MAP         := $(BUILD_DIR)/$(TARGET).map
DISASSEMBLY := $(BUILD_DIR)/$(TARGET).asm


# ==============================================================================
# Source directories
# ==============================================================================

SOURCE_DIRS := \
	arch/$(ARCH) \
	core \
	drivers

INCLUDE_DIRS := \
	include \
	arch/$(ARCH) \
	core \
	drivers

# Add platform-specific source and include directories once they exist.
ifneq ($(wildcard platform/$(PLATFORM)),)
SOURCE_DIRS  += platform/$(PLATFORM)
INCLUDE_DIRS += platform/$(PLATFORM)
endif


# ==============================================================================
# Source discovery
# ==============================================================================

C_SOURCES := $(foreach dir,$(SOURCE_DIRS),$(shell find $(dir) -type f -name '*.c' 2>/dev/null))
S_SOURCES := $(foreach dir,$(SOURCE_DIRS),$(shell find $(dir) -type f -name '*.S' 2>/dev/null))

C_OBJECTS := $(patsubst %.c,$(BUILD_DIR)/%.o,$(C_SOURCES))
S_OBJECTS := $(patsubst %.S,$(BUILD_DIR)/%.o,$(S_SOURCES))

OBJECTS := $(C_OBJECTS) $(S_OBJECTS)
DEPS    := $(OBJECTS:.o=.d)

FORMAT_SOURCES := $(shell find arch core drivers include \
	-type f \( -name '*.c' -o -name '*.h' \) 2>/dev/null)


# ==============================================================================
# Compiler and linker flags
# ==============================================================================

ARCH_FLAGS := \
	-march=$(RISCV_ARCH) \
	-mabi=$(RISCV_ABI) \
	-mcmodel=$(CODE_MODEL)

WARNING_FLAGS := \
	-Wall \
	-Wextra \
	-Werror \
	-Wshadow \
	-Wundef \
	-Wconversion \
	-Wsign-conversion \
	-Wmissing-prototypes \
	-Wstrict-prototypes

FREESTANDING_FLAGS := \
	-ffreestanding \
	-fno-builtin \
	-fno-common \
	-fno-pie \
	-fno-stack-protector \
	-fno-omit-frame-pointer \
	-fno-asynchronous-unwind-tables \
	-fno-unwind-tables \
	-msmall-data-limit=0 \
	-ffunction-sections \
	-fdata-sections

DEPENDENCY_FLAGS := \
	-MMD \
	-MP

INCLUDE_FLAGS := $(addprefix -I,$(INCLUDE_DIRS))

OPT_FLAGS   ?= -O2
DEBUG_FLAGS ?= -g3

CPPFLAGS := $(INCLUDE_FLAGS)

CFLAGS := \
	$(ARCH_FLAGS) \
	$(FREESTANDING_FLAGS) \
	$(WARNING_FLAGS) \
	$(DEPENDENCY_FLAGS) \
	$(OPT_FLAGS) \
	$(DEBUG_FLAGS) \
	-std=c11

ASFLAGS := \
	$(ARCH_FLAGS) \
	$(FREESTANDING_FLAGS) \
	$(DEPENDENCY_FLAGS) \
	$(DEBUG_FLAGS)

LDFLAGS := \
	$(ARCH_FLAGS) \
	-nostdlib \
	-nostartfiles \
	-nodefaultlibs \
	-no-pie \
	-T $(LINKER_SCRIPT) \
	-Wl,--gc-sections \
	-Wl,--build-id=none \
	-Wl,-Map=$(MAP)


# ==============================================================================
# QEMU configuration
# ==============================================================================

QEMU_FLAGS := \
	-machine virt \
	-cpu rv64 \
	-smp $(HARTS) \
	-m $(RAM_SIZE) \
	-nographic \
	-bios default \
	-kernel $(ELF)

QEMU_DEBUG_FLAGS := \
	-S \
	-gdb tcp::1234


# ==============================================================================
# Build targets
# ==============================================================================

.PHONY: all
all: $(ELF)


$(ELF): $(OBJECTS) $(LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) $(OBJECTS) -o $@
	$(SIZE) $@


$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@


$(BUILD_DIR)/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(ASFLAGS) -c $< -o $@


# Include automatically generated header dependencies.
-include $(DEPS)


# ==============================================================================
# Execution and debugging
# ==============================================================================

.PHONY: run
run: $(ELF)
	$(QEMU) $(QEMU_FLAGS)


.PHONY: debug
debug: $(ELF)
	$(QEMU) $(QEMU_FLAGS) $(QEMU_DEBUG_FLAGS)


.PHONY: gdb
gdb: $(ELF)
	$(GDB) \
		-ex "target remote localhost:1234" \
		-ex "break kernel_main" \
		$(ELF)


# ==============================================================================
# Inspection
# ==============================================================================

.PHONY: inspect
inspect: $(ELF)
	$(READELF) -h $(ELF)
	$(READELF) -S $(ELF)
	$(NM) -n $(ELF)


.PHONY: disassemble
disassemble: $(ELF)
	$(OBJDUMP) -d -S $(ELF) > $(DISASSEMBLY)
	@echo "Created $(DISASSEMBLY)"


.PHONY: symbols
symbols: $(ELF)
	$(NM) -n $(ELF)


.PHONY: sections
sections: $(ELF)
	$(READELF) -S $(ELF)


.PHONY: size
size: $(ELF)
	$(SIZE) $(ELF)


# ==============================================================================
# Utility targets
# ==============================================================================

.PHONY: clean
clean:
	rm -rf $(BUILD_DIR)


.PHONY: rebuild
rebuild: clean all


.PHONY: print-config
print-config:
	@echo "Architecture:     $(RISCV_ARCH)"
	@echo "ABI:              $(RISCV_ABI)"
	@echo "Platform:         $(PLATFORM)"
	@echo "Compiler:         $(CC)"
	@echo "Linker script:    $(LINKER_SCRIPT)"
	@echo "Target:           $(ELF)"
	@echo "C sources:        $(C_SOURCES)"
	@echo "Assembly sources: $(S_SOURCES)"


.PHONY: format format-check

format:
	$(CLANG_FORMAT) -i $(FORMAT_SOURCES)

format-check:
	$(CLANG_FORMAT) \
		--dry-run \
		--Werror \
		$(FORMAT_SOURCES)


# ==============================================================================
# Static analysis
# ==============================================================================

TIDY_FLAGS := \
	--target=riscv64-unknown-elf \
	-march=$(RISCV_ARCH) \
	-mabi=$(RISCV_ABI) \
	-mcmodel=$(CODE_MODEL) \
	-ffreestanding \
	-fno-builtin \
	-fno-pie \
	-fno-stack-protector \
	-msmall-data-limit=0 \
	-std=c11 \
	$(INCLUDE_FLAGS)

.PHONY: tidy tidy-fix tidy-config

tidy: $(C_SOURCES)
	@set -e; \
	for source in $(C_SOURCES); do \
		echo "TIDY $$source"; \
		$(CLANG_TIDY) "$$source" -- $(TIDY_FLAGS); \
	done

tidy-fix: $(C_SOURCES)
	@set -e; \
	for source in $(C_SOURCES); do \
		echo "TIDY-FIX $$source"; \
		$(CLANG_TIDY) \
			--fix \
			--fix-errors \
			--format-style=file \
			"$$source" \
			-- $(TIDY_FLAGS); \
	done

tidy-config:
	$(CLANG_TIDY) --verify-config
