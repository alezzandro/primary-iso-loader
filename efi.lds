OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)
SECTIONS
{
  . = 0;
  ImageBase = .;
  .text : { *(.text) *(.text.*) }
  . = ALIGN(4096);
  .data : {
    *(.rodata*)
    *(.data*)
    *(.sdata)
  }
  . = ALIGN(4096);
  .bss : { *(.bss) }
  . = ALIGN(4096);
  .reloc : { *(.reloc) }
  /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.rela*) *(.rel*) }
}
