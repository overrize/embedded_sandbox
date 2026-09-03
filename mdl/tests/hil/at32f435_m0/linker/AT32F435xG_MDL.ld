/*
 * Vendor AT32F435xG_FLASH.ld (AT32F435_437_Firmware_Library_V2.2.6,
 * libraries/cmsis/cm4/device_support/startup/gcc/linker/) with one
 * addition: INCLUDE mdl_arena.ld, wiring in the module sandbox arena
 * (mdl/linker/mdl_arena.ld) right after the host's own .data/.bss/heap
 * layout. Everything else below is the vendor script unmodified.
 *
 * mdl_arena.ld is located via the linker's -L search path (see
 * Makefile's -Wl,-L flag pointing at mdl/linker) rather than copied in,
 * so mdl/linker/mdl_arena.ld stays the single source of truth for the
 * arena layout across every target that includes it.
 */

ENTRY(Reset_Handler)

_estack = 0x20060000;    /* end of RAM */

_Min_Heap_Size = 0x200;
_Min_Stack_Size = 0x400;

MEMORY
{
FLASH (rx)      : ORIGIN = 0x08000000, LENGTH = 1024K
RAM (xrw)       : ORIGIN = 0x20000000, LENGTH = 384K
}

SECTIONS
{
  .isr_vector :
  {
    . = ALIGN(4);
    KEEP(*(.isr_vector))
    . = ALIGN(4);
  } >FLASH

  .text :
  {
    . = ALIGN(4);
    *(.text)
    *(.text*)
    *(.glue_7)
    *(.glue_7t)
    *(.eh_frame)

    KEEP (*(.init))
    KEEP (*(.fini))

    . = ALIGN(4);
    _etext = .;
  } >FLASH

  .rodata :
  {
    . = ALIGN(4);
    *(.rodata)
    *(.rodata*)
    . = ALIGN(4);
  } >FLASH

  .ARM.extab   : { *(.ARM.extab* .gnu.linkonce.armextab.*) } >FLASH
  .ARM : {
    __exidx_start = .;
    *(.ARM.exidx*)
    __exidx_end = .;
  } >FLASH

  .preinit_array     :
  {
    PROVIDE_HIDDEN (__preinit_array_start = .);
    KEEP (*(.preinit_array*))
    PROVIDE_HIDDEN (__preinit_array_end = .);
  } >FLASH
  .init_array :
  {
    PROVIDE_HIDDEN (__init_array_start = .);
    KEEP (*(SORT(.init_array.*)))
    KEEP (*(.init_array*))
    PROVIDE_HIDDEN (__init_array_end = .);
  } >FLASH
  .fini_array :
  {
    PROVIDE_HIDDEN (__fini_array_start = .);
    KEEP (*(SORT(.fini_array.*)))
    KEEP (*(.fini_array*))
    PROVIDE_HIDDEN (__fini_array_end = .);
  } >FLASH

  _sidata = LOADADDR(.data);

  .data :
  {
    . = ALIGN(4);
    _sdata = .;
    *(.data)
    *(.data*)

    . = ALIGN(4);
    _edata = .;
  } >RAM AT> FLASH

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

  /* mdl addition: module sandbox arena, defines __arena_start/__arena_end
   * and __mdl_{text,data,heap_stack,guard}_{start,end} (see
   * mdl/linker/mdl_arena.ld and mdl/core/sandbox.c, which consumes them). */
  INCLUDE mdl_arena.ld

  /DISCARD/ :
  {
    libc.a ( * )
    libm.a ( * )
    libgcc.a ( * )
  }

  .ARM.attributes 0 : { *(.ARM.attributes) }
}
