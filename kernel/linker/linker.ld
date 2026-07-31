/*
 * The main purpose of the linker script is to describe how the sections in
 * the input files should be mapped into the output file, and to control the
 * memory layout of the output file.
 *
 * object files -> linker -> executable
 *
 * each object file has sections and a list of symbols.
 *
 * objdump -h for sections
 * objdump -t or nm for symbols
 *
 * Each section has a name and size and most also have an associated block of
 * data.
 * Each loadable or allocatable output section has two addresses.
 *
 * 1. VMA (virtual memory address): Address which the output file will have
 *    when run.
 * 2. LMA (load memory address): Address at which the section will be loaded.
 *
 * often they are the same.
 *
 * Simple linker script commands:
 * SECTIONS: Used to describe the memory layout of the output file.
 * ENTRY: Set the entry point.
 * PROVIDE: Used to define a symbol only if it is referenced but not defined
 *
 * whitespaces are ignored
 *
 * Source:
 * https://users.informatik.haw-hamburg.de/~krabat/FH-Labor/gnupro/
 * 5_GNUPro_Utilities/c_Using_LD/ldLinker_scripts.html
 *
 */


/*---------------------------------------------------------------------------*/
/* SETUP                                                                     */
/*---------------------------------------------------------------------------*/

/*
 * Specifies the machine architecture .
 */
OUTPUT_ARCH( "riscv" )

/*
 * Specify the entry point (first instruction to execute).
 * The symbole _entry is defined in boot.S
 */
ENTRY ( _entry )


/*---------------------------------------------------------------------------*/
/* Sections                                                                  */
/*---------------------------------------------------------------------------*/
/*
 * This specifies how the input sections map to the output sections.
 * The .= commands set the location counter, and the sections are inserted
 * in increasing address order according to the location counter. "." is the
 * current address.
 * The following statement will take the all of the .bar input sections and
 * relocate them into the .foo output section which starts at address 0x1000.
 *
 *  .= 0x1000;
 *  .foo : { *(.bar) }
 *
 *  Note:
 *  1. ALIGN(0x1000): makes sure that each major region starts on a page boundary.
 *     0x1000 = 4Kib which is the standard RISC-V page size.
 *     This becomes necessary later when we want to do virtual memory because each
     *     page can have separate permissions.
 *  2. NOLOAD: Means that a section occupies memory at runtime but its bytes
 *     do not need to be loaded from the kernel.
 *  3. f. ex.: *(.text .text.*): means: take all input sections named .text or
 *     starting with .text. from every input object file and place them here.
 */

 SECTIONS
 {
    /*
     * QEMU RAM begins at 0x8000_0000 but OpenSBI occupies memory beginning
     * there. So we need to place it at 0x8020_0000 to leave space for the
     * firmware.
     */
    . = 0x80200000;

    PROVIDE ( kernel_start = . );
    /* low addresses */

    /*
     * Contains executable machine instructions.
     * Comes first because OpenSBI jumps to the kernel entry address which
     * makes the boot address predictable.
     * Permissions: readable + executable
     */
    .text ALIGN(0x1000) :
    {
        PROVIDE(text_start = .);

        /*
         * Keep the entry section first even if the linker considers it
         * otherwise unused. If one enables section garbage collection in
         * the linker the linker would otherwise remove this section
         * because it is not called by normal C or assembly functions.
         */
        KEEP(*(.entry_os))

        *(.text .text.*)

        PROVIDE(text_end = .);
    }

    /*
     * Contains read-only data, such as:
     *  - string literals
     *  - constant lookup tables
     *  - const global values
     *  Follows .text because it is also non-writable.
     *  This simplifies memory protection and alginement.
     *  Permissions: readonly
     */
    .rodata ALIGN(0x1000) :
    {
        PROVIDE(rodata_start = .);

        *(.srodata .srodata.*)
        *(.rodata  .rodata.*)

        PROVIDE(rodata_end = .);
    }

    /*
     * Embedded user ELF consumed by the in-memory loader. Keeping it in its
     * own kernel section makes the source image read-only and non-executable.
     */
    .user_image ALIGN(0x1000) :
    {
        PROVIDE(user_program_elf_start = .);

        KEEP(*(.user_image))

        PROVIDE(user_program_elf_end = .);
    }

    /*
     * Contains writable global and static variables with explicit nonzero
     * initial values. Loaded from ELF.
     * Permissions: read + write
     */
    .data ALIGN(0x1000) :
    {
        PROVIDE(data_start = .);

        *(.sdata .sdata.*)
        *(.data  .data.*)

        PROVIDE(data_end = .);
    }

    /*
     * Contains zero-initialized or uninitialized global and static variables.
     * Zeroed at boot.
     * "bss": "Block Started by Symbol"
     * Permissions: read + write
     */
    .bss ALIGN(0x1000) (NOLOAD) : {
        /*
         * We need to define these to clear the area in our assembly startup
         * code.
         */
        PROVIDE(bss_start = .);

        *(.sbss .sbss.*)
        *(.bss  .bss.*)
        /*
         * Collects common symbols. Was used in earlier C versions.
         * It is a good practice to still have it ->
         * makes the linker script more robust.
         */
        *(COMMON)

        PROVIDE(bss_end = .);
    }

    /*
     * The stack is runtime working memory used for:
     *  - local variables
     *  - function arguments
     *  - return addresses
     *  - saved registers
     *  Stack grows downward so the stack pointer begins at the stack's highest
     *  address.
     */
    .stack ALIGN(0x1000) (NOLOAD) : {
        PROVIDE(stack_bottom = .);

        /*
         * Reserve 16 KiB for the initial stack.
         * Choice for early-development. Could get larger in the future.
         */
        . += 16K;

        /*
         * This needs to be loaded into the sp (stack pointer) register
         * by boot.S.
         */
        PROVIDE(stack_top = .);
    }

    /* high addresses */
    PROVIDE ( kernel_end = . );
 }
