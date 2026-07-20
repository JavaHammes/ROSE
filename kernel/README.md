## First goal:

OpenSBI
    - jumps to boot.S in S-mode
    - boot.S establishes the stack
    - boot.S calls kernel_main()
    - kernel_main halts

## How to debug with gdb:

```
qemu-system-riscv64 \
    -machine virt \
    -smp 1 \
    -m 128M \
    -nographic \
    -bios default \
    -kernel kernel.elf \
    -S \
    -gdb tcp::1234
```

In another terminal:
```
riscv64-elf-gdb kernel.elf
```

Then:
```
target remote :1234
break _entry
break kernel_main
continue
```
