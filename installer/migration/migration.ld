/* SPDX-License-Identifier: MIT */

ENTRY(Reset_Handler)

MEMORY
{
    VECTORS (rx)  : ORIGIN = 0x0001c000, LENGTH = 0x1000
    CODE    (rx)  : ORIGIN = 0x0001d000, LENGTH = 0x3e000
    RAM     (rwx) : ORIGIN = 0x20003000, LENGTH = 0x2000
}

SECTIONS
{
    .isr_vector : { KEEP(*(.isr_vector)) } > VECTORS
    .text :
    {
        KEEP(*(.text.Reset_Handler))
        *(.text*)
        *(.rodata*)
        . = ALIGN(4);
    } > CODE
    .bss (NOLOAD) :
    {
        . = ALIGN(4);
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
    } > RAM
    /DISCARD/ : { *(.comment) *(.ARM.attributes) *(.ARM.exidx*) }
}

ASSERT(SIZEOF(.isr_vector) == 0x100, "vector table must contain 64 entries")
ASSERT(ADDR(.text) == 0x0001d000, "migration code must start at 0x1d000")
ASSERT((ADDR(.text) + SIZEOF(.text)) <= 0x0005b000, "migration overlaps metadata")
ASSERT(SIZEOF(.bss) <= 0x2000, "migration RAM overflow")
