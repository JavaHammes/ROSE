# ROSE: Risc-v Operating SystEm

## Current first scope:

- Architecture: RV64
- Machine: QEMU virt (virtual RISC-V computer)
- CPU count: 1 hart (HARdware Thread = 1 virtual CPU core)
- Firmware: QEMU-provided OpenSBI (Open Supervisor Binary Interface. Runs in M-Mode)
- Kernel mode: Supervisor mode
- User mode: -
- Virtual Memory: -
- Interrupts: -
- Device support: UART only

## Memory layout:

- .text: executable instructions
- .rodata: read-only constants
- .data: initialized writable variables
- .bss: zero-initialized writable variables
- stack: function-call and local-variable storage

## Stack:

QEMU virt:
- simulates CPU, RAM, UART, timers, interrupt controllers, etc.

OpenSBI:
- runs in M-mode
- performs machine-level firmware tasks
- provides SBI services to the kernel

Kernel:
- runs in S-mode
- manages memory, processes, traps, drivers, syscalls, scheduling

User programs:
- later runs in U-mode

## Toolchain Setup

```
sudo pacman -Syu 
sudo pacman -S \ 
    base-devel \ 
    riscv64-elf-gcc \ 
    riscv64-elf-gdb \ 
    qemu-system-riscv \ 
    qemu-system-riscv-firmware
```

Verify setup:
```
riscv64-elf-gcc --version
riscv64-elf-as --version
riscv64-elf-ld --version
riscv64-elf-objdump --version
riscv64-elf-readelf --version
riscv64-elf-nm --version
```

```
qemu-system-riscv64 --version
riscv64-elf-gdb --version
qemu-system-riscv64 -machine help | grep virt
```

```
qemu-system-riscv64 \
    -machine virt \
    -smp 1 \
    -m 128M \
    -nographic \
    -bios default
```
