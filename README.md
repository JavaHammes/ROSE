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
- Device-tree discovery for RAM, reserved regions, timer frequency, UART, PLIC,
  and VirtIO-MMIO transports.
- Bitmap physical-page allocator with 4 KiB pages, zero-on-allocation,
  permanent reservations, misuse checks, and a boot self-test.
- Sv39 address spaces with strict kernel RX/R/RW permissions, supervisor-only
  kernel mappings, 2 MiB leaves for low-overhead identity mapping, explicit
  protection changes, transactional range mapping, and intermediate-table
  reclamation.
- Validated ELF64 RISC-V loader with `PT_LOAD` support, BSS zeroing, overlapping
  segment handling, W^X enforcement, and complete unload ownership tracking.
- Reusable modern VirtIO-MMIO transport shared by block, GPU, keyboard, and
  tablet drivers. The 2D GPU driver discovers the scanout, creates an
  XRGB8888-backed resource, and performs bounded dirty-rectangle transfers and
  flushes. A graphical console mirrors `/dev/console` output while UART remains
  available for diagnostics and headless automation.
- A userspace graphics ABI maps the framebuffer into one owning process and
  exposes validated dirty-rectangle flushes and nonblocking input events.
  `/bin/desktop` is a multi-process window server with overlapping surfaces,
  focus and z-order, title-bar dragging, close controls, input routing, and
  damage-tracked composition. Page-backed shared-memory objects carry client
  pixels and event rings. Separate graphical terminal, Files, and live system
  monitor processes render through a compact client UI library and expose
  copy-on-write activity alongside scheduler and memory counters. Screen/input
  ownership and shared mappings are reclaimed automatically on exit, fault,
  or `execve`, restoring the graphical console.
- Generic sector-device interface, modern VirtIO-MMIO block driver, and a
  sixteen-entry write-through cache for 1 KiB filesystem blocks.
- Writable ext2 root filesystem containing `/sbin/init`, `/bin/sh`, programs,
  and data.
  The supported ext2 profile uses one block group, direct and singly indirect
  blocks, 1 KiB blocks, 128-byte inodes, and file-type directory entries. A
  linker-embedded ramfs is retained as a boot-diagnostic fallback.
- VFS regular files, directories, and character devices with canonical path
  lookup, create/truncate, stat, seek, directory iteration, mkdir, and unlink.
- Eight-entry per-process descriptor tables backed by a bounded global
  open-file table, with standard input, output, and error attached to
  `/dev/console`; aliases inherited by `dup`, `fork`, and `spawn` share their
  file offset and remain usable until the final alias is closed. Anonymous
  pipes provide buffered interprocess byte streams, blocking reads and writes,
  EOF and broken-pipe handling, descriptor inheritance across `spawn`, and endpoint
  lifetime tracking across descriptor aliases. Per-descriptor close-on-exec
  and nonblocking flags keep shell-private pipe ends and saved descriptors out
  of child images. Bidirectional pseudo-terminal pairs connect the graphical
  terminal to the existing shell without moving command parsing into the
  window server.
- U-mode C runtime with descriptor I/O, `open`, `close`, `stat`, `fstat`,
  `lseek`, `dup`, `dup2`, directory iteration, `mkdir`, `unlink`, `chdir`,
  `getcwd`, `pipe`, descriptor flags, `fork`, `spawn`, `execve`, `getpid`,
  `waitpid`, `sigaction`, `kill`, process-group and console foreground-group
  control, shared memory, pseudo-terminals, system telemetry, `brk`, `mmap`,
  `mprotect`, `munmap`, `exit`, and `yield`
  system calls. Relative paths resolve from a canonical per-process working
  directory. `brk` provides a private, zero-filled, growable userspace heap
  between the ELF image and stack guard; shrinking or process teardown returns
  its physical pages. A successful `execve` atomically replaces the user image
  while preserving the process identity, working directory, and descriptors
  not marked close-on-exec. `fork` shares immutable pages directly and marks
  writable ELF, heap, and stack pages copy-on-write, returns in both processes,
  and preserves shared open-file descriptions and descriptor flags. `spawn`
  creates a child with copied arguments,
  environment, working directory, and inheritable descriptors; `waitpid`
  supports PID and process-group selectors, exit/signal/stop/continue status,
  and optional nonblocking polling.
  Programs start with conventional `argc`, `argv`, and `envp` values copied to
  their private stack.
  Private anonymous `mmap` reservations allocate zero-filled pages lazily on
  first access. `mprotect` changes complete or partial ranges, including
  no-access mappings, without losing resident contents and preserves W^X and
  copy-on-write isolation. `munmap` releases complete or partial ranges, kernel
  writes into untouched mapped buffers materialize them safely, and `fork`
  carries both untouched reservations and resident pages forward while applying
  copy-on-write to writable pages.
  UART reads and writes block on scheduler wait channels and resume from device
  interrupts without polling in syscall traps.
- Process-directed signals with default termination, ignore, and caught
  dispositions. Caught handlers return through a runtime trampoline and a
  kernel-held register frame, preventing `sigreturn` from forging privileged
  state. `SIGKILL` and `SIGSTOP` remain uncatchable, delivery wakes blocked
  targets, process-group delivery supports terminal jobs, and stop/continue
  transitions are retained for `waitpid`. Dispositions are inherited by
  `fork`, and caught handlers reset on `execve`. The UART maps Ctrl-C and Ctrl-Z
  to the foreground process group; background console reads stop with
  `SIGTTIN`.
- Interactive `/bin/sh` process with console line editing, quoted and escaped
  argument parsing, mutable environment variables, `PATH` lookup, working
  directory built-ins, `<` and truncating `>` redirection, foreground and `&`
  background pipelines of up to six commands, and `jobs`/`fg` job management.
  Practical `/bin` utilities include `ls`, `cat`, `echo`, `pwd`, `env`,
  `mkdir`, and `rm`. Command syntax and dispatch do not run in supervisor mode.
- Sixteen-slot round-robin process scheduler with timer preemption, guarded user
  stacks, per-process kernel trap stacks, process creation, termination,
  parent/child ownership, orphan adoption by PID 0, waiting, reaping, typed
  block/wake channels, and an interruptible idle path with an atomic
  interrupt-masked readiness check. On a disk-root boot, `/sbin/init` remains
  PID 1 while its `/bin/sh` child is running.
- Interrupt-driven UART input, PLIC external interrupts, SBI timers, panic
  diagnostics, and clean shutdown after the userspace shell exits.
- Automated QEMU tests at two RAM sizes, including leak detection.

At idle, the kernel uses five physical pages for its Sv39 tables on the default
QEMU configuration. User-process teardown is checked by the smoke test to
ensure it returns to that baseline.

## Repository layout

```text
kernel/
  arch/riscv64/   boot, trap, context-switch, and embedded-image assembly
  core/           kernel entry, platform parser, traps, ELF, processes
  drivers/        NS16550A UART, PLIC, and VirtIO block drivers
  include/        kernel and shared ABI headers
  linker/         kernel memory layout
  memory/         physical and virtual memory managers
user/             `/bin/sh`, freestanding programs, syscall stubs, linker script
tests/            host-side regressions and end-to-end QEMU smoke test
tools/            deterministic ext2 root-image generator
```

## Requirements

- `riscv64-elf-gcc`, binutils, and GDB
- `qemu-system-riscv64` with the default OpenSBI firmware
- Python 3.9 or newer for host tooling and automated tests
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

For the graphical machine, launch QEMU with the GPU, keyboard, and tablet:

```sh
make run-gui
```

The shell is visible in both the QEMU window and the serial terminal. Run
`desktop` to enter the userspace desktop. Its Terminal runs the same shell over
a pseudo-terminal, Files browses directories, and System Monitor reads live
kernel counters. Click a window to focus it, drag its title bar, use the red
control to close it, and press Escape to return to the serial shell.

After `/sbin/init` launches `/bin/sh` and waits for it, the shell starts at
`rose>`.
Useful commands are:

| Command | Purpose |
| --- | --- |
| `help` | List shell built-ins and command lookup behavior. |
| `echo "hello world"` | Parse quotes and print arguments. |
| `pwd` / `cd [DIR]` | Inspect or change the shell working directory. |
| `ls [DIR]` | List a directory, marking child directories with `/`. |
| `cat [FILE...]` | Copy files, or standard input, to standard output. |
| `mkdir DIR...` / `rm PATH...` | Create or remove filesystem entries. |
| `echo hello > /tmp/message` | Create or truncate a file using redirection. |
| `cat < /tmp/message` | Feed a file to a command as standard input. |
| `echo hello \| cat \| cat` | Run a multi-stage foreground pipeline. |
| `hello` | Resolve `/bin/hello` through `PATH` and run it. |
| `fault` | Verify that U-mode cannot read supervisor kernel text. |
| `syscall-test` | Verify invalid pointers and unknown syscall handling. |
| `console-read` | Block a child until one byte arrives on standard input. |
| `fs-test` | Exercise writable files, directories, stat, and seek. |
| `pipe-test` | Exercise inherited descriptors and blocking anonymous pipes. |
| `desktop` | Enter the multi-application desktop (Escape returns). |
| `env` | Show environment variables inherited by new programs. |
| `setenv NAME VALUE` | Set an inherited environment variable. |
| `unsetenv NAME` | Remove an inherited environment variable. |
| `run [PATH [ARG...]]` | Compatibility alias; defaults to `/bin/hello`. |
| `exit` | Shut QEMU down through SBI SRST. |

## Verification

Fast host-side tests cover the deterministic ext2 image builder and the QEMU
session harness without requiring a cross-toolchain or emulator:

```sh
make test-host
```

Run the normal smoke test:

```sh
make test
```

Run the same kernel with 192 MiB of RAM to verify device-tree-driven memory
discovery:

```sh
make test-platform
```

Boot the VirtIO GPU/input configuration and exercise the userspace mapping and
flush path without opening a host window:

```sh
make test-graphics
```

Run static analysis and both emulator configurations:

```sh
make check
```

For CI or release validation, remove all generated artifacts, rebuild every
kernel and userspace image, and then run the complete check suite:

```sh
make verify
```

The same clean verification runs automatically on pushes and pull requests.
The local pre-commit configuration formats C sources, runs static analysis,
and executes host-side tests for changes in `tests/` or `tools/`.

The smoke tests cover disk-root boot through `/sbin/init` into `/bin/sh`,
userspace line parsing and built-ins, `PATH` execution, VirtIO-backed ext2
mutation, directory utilities, input/output redirection, multi-stage pipelines,
user/kernel isolation, syscall validation, working-directory resolution,
shared descriptor offsets across aliases and processes, close-on-exec and
nonblocking inheritance, shared-memory visibility and cleanup,
pseudo-terminal duplex I/O, system telemetry, userspace heap growth and
shrinkage, lazy anonymous mappings, partial protection and unmapping, mapping
protection and reclamation, copy-on-write `fork` address-space isolation and
concurrent process-capacity stress, caught/ignored/default signal delivery,
signal wakeup of blocked processes, fork inheritance and exec reset of
dispositions, blocking UART and pipe I/O, process-group signal delivery, stop/continue wait
events, Ctrl-C and Ctrl-Z foreground handling, background commands, `jobs`/`fg`,
child creation and waiting, memory reclamation, fallback-ramfs shell boot,
graphical transport discovery, userspace framebuffer mapping and flushing,
automatic graphical console restoration, and clean shutdown. The graphical
smoke test boots and closes the full three-client desktop; the terminal path
was also verified with a shell command entered through its pseudo-terminal.

The host-side image tests additionally validate filesystem metadata, output
determinism, atomic replacement, guest path constraints, and malformed tree
rejection. Invalid image-builder mappings are reported as concise usage errors
instead of Python tracebacks.

For source-level debugging, use `make debug` in one terminal and `make gdb` in
another. `make disassemble` writes an annotated disassembly to
`kernel/build/kernel.asm`.

## Boot and execution flow

1. OpenSBI enters `_entry` in supervisor mode and supplies the hart ID and DTB.
2. Assembly establishes the boot stack, clears BSS, and calls `kernel_main`.
3. The kernel parses the DTB, initializes physical memory, builds the kernel
   Sv39 address space, and enables interrupts and console I/O.
4. The kernel mounts its embedded diagnostic ramfs, negotiates the modern
   VirtIO-MMIO block device, validates ext2, and replaces the VFS root with the
   disk filesystem. `/dev/console` remains a VFS character-device overlay.
5. `/sbin/init` is read from disk as the first user process, spawns `/bin/sh`,
   and waits to reap it; the fallback ramfs starts `/bin/sh` directly. A process
   resolving any later executable receives a private Sv39 root, user stack,
   kernel trap stack, and pages populated from that ELF.
6. Traps switch from the untrusted user stack to the process kernel stack.
   Syscalls, faults, yields, and timer ticks may update the saved trap frame.
7. `/bin/sh` reads and edits console input, parses commands and redirections,
   handles built-ins, or connects `PATH`-resolved children into a process-group
   pipeline. It gives foreground jobs the console, retains stopped/background
   jobs, and reclaims the console before prompting. Blocking console I/O, pipe
   I/O, and `waitpid` retain their `ecall` and resume through a fresh trap after
   their wait channel is woken. If every runnable process is blocked or
   stopped, the kernel scheduler waits in supervisor mode with `wfi`.
8. Exiting `/bin/sh` wakes `/sbin/init`, which reaps the shell and exits. The
   kernel then verifies that all user-owned pages were reclaimed and requests
   SBI system shutdown. The fallback shell returns to the kernel directly.

## Deliberate limitations

ROSE currently targets one hart and one QEMU `virt` platform. The shell has no
append redirection, descriptor number syntax, expansion, `bg` command, session
management, or scripting language. The ext2 implementation is synchronous,
write-through, limited to one block group and direct plus singly indirect block
addressing (268 KiB files), and does not yet provide journaling or crash
recovery. The VirtIO queue is polled synchronously; it is not yet connected to
a scheduler completion channel. Signals do not yet have masks, alternate
stacks, sessions, or realtime queues. Anonymous mappings use a fixed
sixteen-entry VMA table and do not yet support file backing, shared mappings,
or fixed addresses. There are no general-purpose kernel threads, ASIDs,
networking, users, or permissions enforcement.
