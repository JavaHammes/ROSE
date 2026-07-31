OUTPUT_ARCH("riscv")
ENTRY(_start)

/*
 * Produce separate loadable RX and RW segments. The kernel derives page
 * permissions from these program headers and rejects any W+X result.
 */
PHDRS
{
    text PT_LOAD FLAGS(5);
    data PT_LOAD FLAGS(6);
    stack PT_GNU_STACK FLAGS(6);
}

SECTIONS
{
    /* Keep low canonical addresses free for a null guard and future metadata. */
    . = 0x00400000;
    user_image_start = .;

    .text :
    {
        KEEP(*(.text.entry))
        *(.text .text.*)
    } :text

    .rodata ALIGN(16) :
    {
        *(.srodata .srodata.*)
        *(.rodata .rodata.*)
    } :text

    /* Begin writable data on a fresh page so it never shares an RX leaf. */
    . = ALIGN(0x1000);

    .data :
    {
        *(.sdata .sdata.*)
        *(.data .data.*)
    } :data

    /* NOLOAD contributes memory_size but no file bytes; the ELF loader's
     * zero-filled pages provide the initial BSS contents. */
    .bss ALIGN(16) (NOLOAD) :
    {
        *(.sbss .sbss.*)
        *(.bss .bss.*)
        *(COMMON)
    } :data

    user_image_end = .;

    /* Leave 0x007ff000 unmapped as the guard below the fixed user stack. */
    ASSERT(user_image_end < 0x007ff000,
           "user program overlaps the stack guard")

    /* These toolchain metadata sections are not required at runtime. */
    /DISCARD/ :
    {
        *(.comment)
        *(.riscv.attributes)
    }
}
