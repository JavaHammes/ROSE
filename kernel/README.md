## First goal:

OpenSBI
    -> jumps to boot.S in S-mode
    -> boot.S establishes the stack
    -> boot.S calls kernel_main()
    -> kernel_main prints through the UART
    -> kernel halts
