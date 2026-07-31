/*
* This RISC-V assembly script does the following:
*
* 1. Place _entry in .entry_os
* 2. Export _entry as a global symbol
* 3. Set sp to stack_top
* 4. Preserve the OpenSBI boot arguments:
* 	- a0 = hart ID
*	- a1 = Device Tree Blob (DTB) address
*           - The Device Tree describes the virtual hardware that QEMU created, including:
*               - RAM location and size
*               - CPU and hart information
*               - UART address
*               - timer hardware
*               - interrupt controller
*               - virtio devices
* 5. Clear memory from bss_start to bss_end
* 6. Call C entry function
* 7. Enter an infinite halt loop if the C function returns.
* 
* Structure of this code from:
* https://github.com/RISCV-on-Microsemi-FPGA/Operating-Systems/blob/master/FreeRTOS/FreeRTOS_on_Spike/Demo/riscv-spike/arch/boot.S
*/


/*
 * This tells the assembler to place subsequent instructions into the .entry_os section.
 * The flags mean:
 *  - a: allocate this section in memory
 *  - x: the section contains executable code
 *  - @progbits: the section contains actual bytes in the ELF file
 */
.section .entry_os,"ax",@progbits

/*
 * Exports _entry as a global symbol (which exposes it to the linker)
 */
.globl _entry

/*
 * Marks _entry and fill_block as a function symbols for ELF/debugging tools.
 */
.type _entry, @function

/*
 * Startup code begins here
 */
_entry:

    // init stack
    la  sp, stack_top

    // preserve OpenSBI boot arguments
    mv  s0, a0
    mv  s1, a1

    // clear bss:
    la  a0, bss_start
    la  a1, bss_end
    call fill_block

    // restore OpenSBI boot arguments
    mv  a0, s0
    mv  a1, s1

    /* call C entry
     *                                     a0,              a1
     * void kernel_main(unsigned long hart_id, const void *dtb);
     */
    call kernel_main

// halt (do nothing) until infinity
halt:
    wfi                         // wfi: wait for interrupt
    j  halt

/*
 * Record the size of the _entry function in the ELF symbol table.
 * "." is the current address, so ". - _entry" is the number of bytes
 * from the start of the _entry to this point.
 * (only useful for debugging purposes)
 */
.size _entry, . - _entry

/*
 * Switch to ordinary .text section
 */
.section .text,"ax",@progbits
.type fill_block, @function

/*
 * Start of the helper routine that clears a memory range.
 *
 * Input:
 *  - a0: current destination address
 *  - a1: exclusive end address
 */
fill_block:
    bgeu  a0, a1, fb_end        // branch if greater or equal, unsigned. stop if current address is greater or equal to the exclusive end address.
    sb    zero, 0(a0)           // store one zero byte at the address currently held in a0. The "zero" register always contains the value 0.
    addi  a0, a0, 1             // advance the destination address by one byte.
    j     fill_block            // j translates into jal x0, fill_block (x0 is the zero register means we don't care about the return address).
fb_end:
    ret

.size fill_block, . - fill_block
