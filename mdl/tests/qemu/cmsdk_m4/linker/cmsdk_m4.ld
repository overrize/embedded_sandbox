/*
 * QEMU `-M mps2-an386` memory map -- CODE at 0x00000000, RAM at
 * 0x20000000, per standard ARMv7-M/CMSDK convention (candidate values,
 * to be confirmed empirically by actually booting on this exact QEMU
 * build -- see mdl/tests/qemu/cmsdk_m4/README.md for the boot-test
 * result this was verified against).
 */
ENTRY(Reset_Handler)

_estack = 0x20080000; /* 512K RAM, adjust if the boot test shows a different size */

_Min_Heap_Size = 0x200;
_Min_Stack_Size = 0x400;

MEMORY
{
CODE (rx)  : ORIGIN = 0x00000000, LENGTH = 4096K
RAM  (xrw) : ORIGIN = 0x20000000, LENGTH = 512K
}

/* See mdl/tests/hil/at32f435_m2/linker/AT32F435xG_MDL_MPU.ld's top
 * comment for what these are and the ALIGN()-must-be-inside-the-section
 * gotcha this already accounts for. */
__FLASH_segment_start__ = ORIGIN(CODE);
__FLASH_segment_end__   = ORIGIN(CODE) + LENGTH(CODE);
__SRAM_segment_start__  = ORIGIN(RAM);
__SRAM_segment_end__    = ORIGIN(RAM) + LENGTH(RAM);

SECTIONS
{
  .isr_vector :
  {
    . = ALIGN(4);
    KEEP(*(.isr_vector))
    . = ALIGN(4);
  } >CODE

  .privileged_functions :
  {
    . = ALIGN(32768);
    __privileged_functions_start__ = .;
    *(privileged_functions)
    . = ALIGN(32768);
    __privileged_functions_end__ = .;
  } >CODE

  .freertos_system_calls :
  {
    . = ALIGN(8192);
    __syscalls_flash_start__ = .;
    *(freertos_system_calls)
    . = ALIGN(8192);
    __syscalls_flash_end__ = .;
  } >CODE

  .text :
  {
    . = ALIGN(4);
    *(.text)
    *(.text*)
    KEEP (*(.init))
    KEEP (*(.fini))
    . = ALIGN(4);
    _etext = .;
  } >CODE

  .rodata :
  {
    . = ALIGN(4);
    *(.rodata)
    *(.rodata*)
    . = ALIGN(4);
  } >CODE

  .ARM.extab   : { *(.ARM.extab* .gnu.linkonce.armextab.*) } >CODE
  .ARM : {
    __exidx_start = .;
    *(.ARM.exidx*)
    __exidx_end = .;
  } >CODE

  .preinit_array : {
    PROVIDE_HIDDEN (__preinit_array_start = .);
    KEEP (*(.preinit_array*))
    PROVIDE_HIDDEN (__preinit_array_end = .);
  } >CODE
  .init_array : {
    PROVIDE_HIDDEN (__init_array_start = .);
    KEEP (*(SORT(.init_array.*)))
    KEEP (*(.init_array*))
    PROVIDE_HIDDEN (__init_array_end = .);
  } >CODE
  .fini_array : {
    PROVIDE_HIDDEN (__fini_array_start = .);
    KEEP (*(SORT(.fini_array.*)))
    KEEP (*(.fini_array*))
    PROVIDE_HIDDEN (__fini_array_end = .);
  } >CODE

  _sidata = LOADADDR(.data);

  .privileged_data :
  {
    . = ALIGN(32768);
    __privileged_data_start__ = .;
    *(privileged_data)
    . = ALIGN(32768);
    __privileged_data_end__ = .;
  } >RAM AT> CODE

  .data :
  {
    . = ALIGN(4);
    _sdata = .;
    *(.data)
    *(.data*)
    . = ALIGN(4);
    _edata = .;
  } >RAM AT> CODE

  . = ALIGN(4);
  .bss :
  {
    _sbss = .;
    __bss_start__ = _sbss;
    *(.bss)
    *(.bss*)
    *(COMMON)
    . = ALIGN(4);
    _ebss = .;
    __bss_end__ = _ebss;
  } >RAM

  ._user_heap_stack :
  {
    . = ALIGN(8);
    PROVIDE ( end = . );
    PROVIDE ( _end = . );
    . = . + _Min_Heap_Size;
    . = . + _Min_Stack_Size;
    . = ALIGN(8);
  } >RAM

  INCLUDE mdl_arena.ld

  /DISCARD/ :
  {
    libc.a ( * )
    libm.a ( * )
    libgcc.a ( * )
  }

  .ARM.attributes 0 : { *(.ARM.attributes) }
}
