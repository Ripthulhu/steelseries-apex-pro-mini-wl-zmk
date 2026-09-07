ENTRY(entry)
MEMORY { FLASH (rx) : ORIGIN = 0x26000, LENGTH = 0x17000 }
SECTIONS {
    .text : { KEEP(*(.entry)) *(.text*) *(.rodata*) } > FLASH
    /DISCARD/ : { *(.comment*) *(.ARM.exidx*) *(.ARM.extab*) }
}

