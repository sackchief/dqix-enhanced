SECTIONS {
  . = 0x1446be000;
  .text : { *(.text*) }
  .rodata : { *(.rodata*) }
  .got : { *(.got*) }
  .data : { *(.data*) *(.bss*) *(COMMON) }
  /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
