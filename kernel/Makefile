CROSS := riscv64-elf-

CC      := $(CROSS)gcc
OBJDUMP := $(CROSS)objdump
READELF := $(CROSS)readelf
NM      := $(CROSS)nm

ARCH_FLAGS := -march=rv64imac -mabi=lp64 -mcmodel=medany

CFLAGS := $(ARCH_FLAGS) \
          -ffreestanding \
          -fno-stack-protector \
          -fno-pie \
          -msmall-data-limit=0 \
          -Wall \
          -Wextra \
          -O0 \
          -g

ASFLAGS := $(ARCH_FLAGS) \
           -ffreestanding \
           -fno-pie \
           -g

LDFLAGS := $(ARCH_FLAGS) \
           -nostdlib \
           -nostartfiles \
           -no-pie \
           -T linker.ld \
           -Wl,--gc-sections \
           -Wl,-Map=kernel.map

OBJECTS := boot.o kernel.o uart.o

.PHONY: all run inspect clean

all: kernel.elf

uart.o: uart.c
	$(CC) $(ASFLAGS) -c $< -o $@

boot.o: boot.S
	$(CC) $(ASFLAGS) -c $< -o $@

kernel.o: kernel.c
	$(CC) $(CFLAGS) -c $< -o $@

kernel.elf: $(OBJECTS) linker.ld
	$(CC) $(LDFLAGS) $(OBJECTS) -o $@

run: kernel.elf
	qemu-system-riscv64 \
		-machine virt \
		-smp 1 \
		-m 128M \
		-nographic \
		-bios default \
		-kernel kernel.elf

inspect: kernel.elf
	$(READELF) -h kernel.elf
	$(READELF) -S kernel.elf
	$(NM) -n kernel.elf
	$(OBJDUMP) -d kernel.elf

clean:
	rm -f $(OBJECTS) kernel.elf kernel.map
