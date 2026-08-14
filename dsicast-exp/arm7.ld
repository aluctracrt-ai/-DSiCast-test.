OUTPUT_FORMAT("elf32-littlearm")
OUTPUT_ARCH(arm)
ENTRY(_start)
MEMORY { ram (rwx) : ORIGIN = 0x02FC8000, LENGTH = 0x0F000 }
SECTIONS {
  . = ORIGIN(ram);
  .init : { KEEP(*(.init)) } > ram
  .text : { *(.text*) *(.glue_7*) *(.glue_7t*) } > ram
  .rodata : { *(.rodata*) *(.gnu.linkonce.r*) } > ram
  .ARM.extab : { *(.ARM.extab*) } > ram
  .ARM.exidx : { __exidx_start = .; *(.ARM.exidx*) __exidx_end = .; } > ram
  .data : { *(.data*) *(.gnu.linkonce.d*) } > ram
  .bss (NOLOAD) : { __bss_start__ = .; *(.bss*) *(COMMON) __bss_end__ = .; } > ram
  __image_end__ = .;
  /DISCARD/ : { *(.comment*) *(.debug*) *(.note*) }
}
ASSERT(__image_end__ < 0x02FD7000, "DSiCast ARM7 resident too large (stack collision)")
