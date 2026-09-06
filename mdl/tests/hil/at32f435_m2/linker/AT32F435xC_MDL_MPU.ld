/*
 * M2 linker script: the M0/M1 AT32F435xG_MDL.ld base, plus three new
 * output sections FreeRTOS-MPU's port.c needs (prvSetupMPU() reads
 * these symbols directly -- see mdl/third_party/freertos_mpu_port):
 *
 *   .privileged_functions   FLASH, execute-privileged-only (region 1)
 *   .privileged_data        RAM,   privileged-only          (region 2)
 *   .freertos_system_calls  FLASH, unprivileged-EXECUTABLE   (no region
 *                           of its own -- covered by the default flash
 *                           region 0 -- but its bounds gate
 *                           configENFORCE_SYSTEM_CALLS_FROM_KERNEL_ONLY:
 *                           only code whose PC falls inside
 *                           __syscalls_flash_start__/_end__ is allowed
 *                           to actually raise privilege via SVC)
 *
 * ARMv7-M MPU alignment rule (base address aligned to region size,
 * region size a power of two) applies to these two kernel-fixed regions
 * exactly like it does to the module arena.
 *
 * privileged_functions/privileged_data are NOT "a handful of wrapper
 * functions" -- tasks.c/queue.c/timers.c/list.c/event_groups.c/
 * stream_buffer.c each define MPU_WRAPPERS_INCLUDED_FROM_API_FILE
 * before including task.h, which makes PRIVILEGED_FUNCTION/
 * PRIVILEGED_DATA expand to real section attributes for THEIR OWN
 * (non-MPU_-prefixed) code -- i.e. this is essentially the entire real
 * FreeRTOS kernel implementation and its internal state, not just the
 * thin MPU_xxx gate functions (those, plus our own host_api.c gates,
 * are what freertos_system_calls holds). Real measured content:
 * privileged_functions ~19.5K, privileged_data ~17K, freertos_system_calls
 * ~4K -- comfortably under the ALIGN(32768)/ALIGN(32768)/ALIGN(8192)
 * budgets below.
 *
 * GOTCHA (spent real time on this, verified empirically against this
 * exact ld, not assumed): a bare ". = ALIGN(N);" statement BETWEEN two
 * output section blocks, at the top level of SECTIONS{}, does NOT move
 * where the next section starts -- confirmed by testing it three ways
 * (ALIGN(N), a literal hex address, and checking the unfiltered
 * --print-map output) and getting the SAME unaligned start every time.
 * What DOES work: put ". = ALIGN(N);" as the FIRST statement INSIDE the
 * section body, before the symbol/content it's meant to align. The
 * linker fixes an output section's own placement at the point "."
 * stood when it entered that section's braces; assignments inside the
 * braces (including the END-of-section ALIGN(), which behaves as
 * expected either way) shift the running "." from there, but an
 * assignment made BEFORE the braces doesn't retroactively move the
 * section's start -- it just gets skipped over. Net effect: each of
 * these three sections absorbs its own leading padding internally
 * (visible as the section's reported size being inflated by the gap),
 * rather than the gap appearing as empty space between sections. Wastes
 * a little flash to alignment padding; functionally correct symbols
 * either way, since prvSetupMPU()/vPortStoreTaskMPUSettings() only ever
 * read the __*_start__/__*_end__ SYMBOLS, never a section's own sh_addr.
 */

ENTRY(Reset_Handler)

_estack = 0x20060000;

_Min_Heap_Size = 0x200;
_Min_Stack_Size = 0x400;

MEMORY
{
/* AT32F435VCT7 (LQFP100): 256K flash, 384K SRAM -- 与厂商 AT32F435xC_FLASH.ld 一致 */
FLASH (rx)      : ORIGIN = 0x08000000, LENGTH = 256K
RAM (xrw)       : ORIGIN = 0x20000000, LENGTH = 384K
}

/*
 * port.c's prvSetupMPU() also wants the FULL extent of flash/RAM, not
 * just the privileged carve-outs -- portUNPRIVILEGED_FLASH_REGION
 * (region 0) is "all of flash, read+execute, EVERYONE" (this is what
 * makes freertos_system_calls's gate functions and ordinary module
 * .text fetchable by unprivileged code in the first place), and
 * vPortStoreTaskMPUSettings() uses __SRAM_segment_*__ as "all of RAM"
 * for any task NOT created via xTaskCreateRestricted() (xRegions==NULL)
 * -- i.e. the loader task, idle task, and timer task, which legitimately
 * need broad RAM access and are never untrusted module code. A
 * restricted task (module_task.c's xRegions != NULL) never takes this
 * path -- it always gets exactly the 3 regions it was given, nothing
 * broader.
 */
__FLASH_segment_start__ = ORIGIN(FLASH);
__FLASH_segment_end__   = ORIGIN(FLASH) + LENGTH(FLASH);
__SRAM_segment_start__  = ORIGIN(RAM);
__SRAM_segment_end__    = ORIGIN(RAM) + LENGTH(RAM);

SECTIONS
{
  .isr_vector :
  {
    . = ALIGN(4);
    KEEP(*(.isr_vector))
    . = ALIGN(4);
  } >FLASH

  .privileged_functions :
  {
    /* ALIGN() must be inside the braces -- see the GOTCHA note up top. */
    . = ALIGN(32768);
    __privileged_functions_start__ = .;
    *(privileged_functions)
    . = ALIGN(32768);
    __privileged_functions_end__ = .;
  } >FLASH

  .freertos_system_calls :
  {
    . = ALIGN(8192);
    __syscalls_flash_start__ = .;
    *(freertos_system_calls)
    . = ALIGN(8192);
    __syscalls_flash_end__ = .;
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

  /*
   * ============================================================
   * .privileged_data MUST be inside the startup's copy range.
   * ============================================================
   *
   * This was the bug behind the first real-hardware BusFault (2026-09-06)
   * and, almost certainly, the long-standing QEMU PendSV blocker too.
   *
   * The vendor startup (startup_at32f435_437.s) does exactly two things
   * to RAM: copy _sidata -> [_sdata, _edata), and zero [_sbss, _ebss).
   * As originally written, this script computed
   *
   *     _sidata = LOADADDR(.data);
   *
   * and put _sdata at the start of .data -- so the 32KB .privileged_data
   * section sitting between the two was covered by NEITHER loop, even
   * though it is a loaded section with real initialisers in flash. Every
   * FreeRTOS PRIVILEGED_DATA object (pxCurrentTCB, pxReadyTasksLists[],
   * uxCurrentNumberOfTasks, uxCriticalNesting -- whose initialiser is
   * 0xaaaaaaaa, not 0 -- and heap_4's ucHeap) therefore came up holding
   * whatever was in SRAM at power-on.
   *
   * On hardware that meant pxCurrentTCB was garbage-non-NULL, so
   * prvInitialiseTaskLists() never ran, so the first xTaskCreate() walked
   * an uninitialised list and bus-faulted in vListInsertEnd. Under QEMU
   * the same section came up all-zero instead, which accidentally fixes
   * pxCurrentTCB but silently breaks uxCriticalNesting -- a different
   * symptom from the same root cause.
   *
   * The fix: _sdata now marks the start of .privileged_data, not .data,
   * and _sidata is that section's load address. Both sections are
   * consecutive `AT> FLASH`, so their load addresses are contiguous and
   * one memcpy still covers both. Do not move _sdata back into .data,
   * and do not insert another loaded section between these two without
   * extending the range again.
   *
   * Cost: the trailing ALIGN(32768) pads .privileged_data out to a full
   * 32KB that now actually gets copied at boot. That padding was already
   * being stored in flash before this change; only the copy is new, and
   * a 32KB memcpy at 288MHz is not measurable next to USB enumeration.
   */
  _sidata = LOADADDR(.privileged_data);

  . = ALIGN(32768);
  .privileged_data :
  {
    . = ALIGN(32768);
    __privileged_data_start__ = .;
    _sdata = .;                     /* copy range starts HERE, not at .data */
    *(privileged_data)
    . = ALIGN(32768);
    __privileged_data_end__ = .;
  } >RAM AT> FLASH

  .data :
  {
    . = ALIGN(4);
    *(.data)
    *(.data*)

    . = ALIGN(4);
    _edata = .;                     /* ... and ends at the end of .data */
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

  INCLUDE mdl_arena.ld

  /DISCARD/ :
  {
    libc.a ( * )
    libm.a ( * )
    libgcc.a ( * )
  }

  .ARM.attributes 0 : { *(.ARM.attributes) }
}
