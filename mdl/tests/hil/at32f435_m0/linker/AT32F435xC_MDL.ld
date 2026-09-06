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
/* AT32F435VCT7 (LQFP100, UYUP-RPI-A-2.4 底板): 256K flash, 384K SRAM.
 * 与厂商 libraries/cmsis/cm4/device_support/startup/gcc/linker/AT32F435xC_FLASH.ld
 * 的 MEMORY 块一致 -- 那是这两个数字的权威来源, 不要凭型号名猜。 */
FLASH (rx)      : ORIGIN = 0x08000000, LENGTH = 256K
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

  /*
   * Deliberately OUTSIDE _sbss.._ebss, so startup's zero loop skips it:
   * this is the only RAM whose contents are expected to survive a reset.
   *
   * It exists because a fault on this board is self-erasing. A module
   * fault parks the CPU in a handler, USB stops answering, the host
   * drops the port, and whatever the handler recorded dies with the next
   * reset -- so the one run that had the answer is also the one run that
   * cannot report it. Recording the fault here and resetting on purpose
   * turns that into: crash, come back, and read it out over the console
   * (`fault`). Anything placed here MUST tolerate powering up as
   * uninitialised garbage; board_debug.c's record carries its own magic
   * for exactly that reason.
   */
  .noinit (NOLOAD) :
  {
    . = ALIGN(4);
    __noinit_start__ = .;
    *(.noinit)
    *(.noinit*)
    . = ALIGN(4);
    __noinit_end__ = .;
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
