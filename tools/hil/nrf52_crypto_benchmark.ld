ENTRY(bench_start)
MEMORY { RAM (rwx) : ORIGIN = 0x20000000, LENGTH = 128K }
SECTIONS {
 .text : { KEEP(*(.entry)) *(.text*) *(.rodata*) } > RAM
 .ARM.exidx : { *(.ARM.exidx*) } > RAM
 .data : { *(.data*) } > RAM
 .bss (NOLOAD) : { *(.bss*) *(COMMON) } > RAM
 /DISCARD/ : { *(.comment*) *(.note*) }
}
