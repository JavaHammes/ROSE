# ROSE — RISC-V Operating System

ROSE is a compact educational kernel for the 64-bit RISC-V QEMU `virt`
machine. It boots through OpenSBI, discovers its platform from the flattened
device tree, enables Sv39 virtual memory, and runs independently linked C
programs in user mode.

The project is intentionally small enough to read end to end, but its current
milestone has real protection boundaries, resource ownership, preemptive
scheduling, and automated emulator tests.

## Current features

- RV64 supervisor-mode kernel on one hart.
- Device-tree discovery for RAM, reserved regions, timer frequency, UART, UART
  interrupt, and PLIC.
- Bitmap physical-page allocator with 4 KiB pages, zero-on-allocation,
  permanent reservations, misuse checks, and a boot self-test.
- Sv39 address spaces with strict kernel RX/R/RW permissions, supervisor-only
  kernel mappings, 2 MiB leaves for low-overhead identity mapping, explicit
  protection changes, transactional range mapping, and intermediate-table
  reclamation.
- Validated ELF64 RISC-V loader with `PT_LOAD` support, BSS zeroing, overlapping
  segment handling, W^X enforcement, and complete unload ownership tracking.
- U-mode C runtime with `write`, `exit`, and `yield` system calls.
- Eight-slot round-robin process scheduler with timer preemption, guarded user
  stacks, per-process kernel trap stacks, process creation, termination,
  waiting, and reaping.
- Interrupt-driven UART input, PLIC external interrupts, SBI timers, panic
  diagnostics, and SBI system shutdown.
- Automated QEMU tests at two RAM sizes, including leak detection.

At idle, the kernel uses five physical pages for its Sv39 tables on the default
QEMU configuration. User-process teardown is checked by the smoke test to
ensure it returns to that baseline.

## Repository layout

```text
kernel/
  arch/riscv64/   boot, trap, context-switch, and embedded-image assembly
  core/           kernel entry, platform parser, shell, traps, ELF, processes
  drivers/        NS16550A UART and PLIC drivers
  include/        kernel and shared ABI headers
  linker/         kernel memory layout
  memory/         physical and virtual memory managers
user/             freestanding C test program, runtime stubs, and linker script
tests/            end-to-end QEMU smoke test
```

## Requirements

- `riscv64-elf-gcc`, binutils, and GDB
- `qemu-system-riscv64` with the default OpenSBI firmware
- Python 3 for automated tests
- `clang-tidy` for static analysis

On Arch Linux, the relevant packages are `riscv64-elf-gcc`,
`riscv64-elf-binutils`, `riscv64-elf-gdb`, `qemu-system-riscv`, and
`qemu-system-riscv-firmware`. Package names differ on macOS and other Linux
distributions.

The tool prefix and executables can be overridden when necessary:

```sh
make PREFIX=riscv64-unknown-elf- QEMU=qemu-system-riscv64
```

## Build and run

```sh
make -j4
make run
```

The terminal starts at `rose>`. Useful commands are:

| Command | Purpose |
| --- | --- |
| `help` | List every command. |
| `info` | Show the discovered platform and virtual-memory state. |
| `meminfo` | Show usable, used, and free physical pages. |
| `run` | Run the normal U-mode C program. |
| `run fault` | Verify that U-mode cannot read supervisor kernel text. |
| `run syscall` | Verify invalid pointers and unknown syscall handling. |
| `runmulti` | Run two timer-preempted processes. |
| `spawn [hello\|fault\|a\|b\|syscall]` | Create a ready process. |
| `wait` | Run all ready processes until they exit. |
| `kill PID` | Terminate a ready process. |
| `ps` | Show ready and exited process-table entries. |
| `reap` | Remove exited entries and make their slots reusable. |
| `exit` | Shut QEMU down through SBI SRST. |

## Verification

Run the normal smoke test:

```sh
make test
```

Run the same kernel with 192 MiB of RAM to verify device-tree-driven memory
discovery:

```sh
make test-platform
```

Run static analysis and both emulator configurations:

```sh
make check
```

The smoke test covers boot, discovered hardware, the idle page-table budget,
ELF execution, user/kernel isolation, syscall validation, preemption, process
lifecycle commands, memory reclamation, and clean shutdown.

For source-level debugging, use `make debug` in one terminal and `make gdb` in
another. `make disassemble` writes an annotated disassembly to
`kernel/build/kernel.asm`.

## Boot and execution flow

1. OpenSBI enters `_entry` in supervisor mode and supplies the hart ID and DTB.
2. Assembly establishes the boot stack, clears BSS, and calls `kernel_main`.
3. The kernel parses the DTB, initializes physical memory, builds the kernel
   Sv39 address space, and enables interrupts and the terminal.
4. A process receives a private Sv39 root, user stack, kernel trap stack, and
   pages populated from the embedded ELF image.
5. Traps switch from the untrusted user stack to the process kernel stack.
   Syscalls, faults, yields, and timer ticks may update the saved trap frame.
6. When no ready process remains, execution returns to the foreground shell;
   exited processes retain only status metadata until `reap`.

## Deliberate limitations

ROSE currently targets one hart and one QEMU `virt` platform. User workloads
run as a foreground batch while the kernel still maintains persistent ready
and exited states between shell commands. There is one embedded user ELF, no
filesystem or persistent storage, no blocking I/O or sleep queues, no ASIDs,
and no networking.
