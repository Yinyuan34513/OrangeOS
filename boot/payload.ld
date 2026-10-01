OUTPUT_FORMAT(elf32-i386)
OUTPUT_ARCH(i386)
ENTRY(c_start)

/* The payload is loaded at 0x10000 by boot.asm, and c_start has to be the
 * first byte there, hence its own section pinned ahead of everything else. */
SECTIONS
{
    . = 0x10000;

    .text : {
        KEEP(*(.text.c_start))
        *(.text .text.*)
    }

    .rodata : { *(.rodata .rodata.*) }
    .data   : { *(.data .data.*) }
    .bss    : { *(.bss .bss.*) *(COMMON) }

    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
