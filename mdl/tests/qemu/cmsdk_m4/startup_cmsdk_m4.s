/*
 * Minimal Cortex-M4 startup for the QEMU `-M mps2-an386` target.
 * Trimmed from the AT32 vendor startup (same structure: copy .data,
 * zero .bss, call SystemInit()+__libc_init_array(), branch to main())
 * but with only the core exceptions in the vector table -- this target
 * doesn't use any CMSDK peripheral IRQ (UART/timer/etc) yet.
 */
  .syntax unified
  .cpu cortex-m4
  .fpu softvfp
  .thumb

.global g_pfnVectors
.global Default_Handler

.word _sidata
.word _sdata
.word _edata
.word _sbss
.word _ebss

  .section .text.Reset_Handler
  .weak Reset_Handler
  .type Reset_Handler, %function
Reset_Handler:
  movs r1, #0
  b LoopCopyDataInit
CopyDataInit:
  ldr r3, =_sidata
  ldr r3, [r3, r1]
  str r3, [r0, r1]
  adds r1, r1, #4
LoopCopyDataInit:
  ldr r0, =_sdata
  ldr r3, =_edata
  adds r2, r0, r1
  cmp r2, r3
  bcc CopyDataInit
  ldr r2, =_sbss
  b LoopFillZerobss
FillZerobss:
  movs r3, #0
  str r3, [r2], #4
LoopFillZerobss:
  ldr r3, =_ebss
  cmp r2, r3
  bcc FillZerobss
  bl SystemInit
  bl __libc_init_array
  bl main
  bx lr
  .size Reset_Handler, .-Reset_Handler

  .section .text.Default_Handler,"ax",%progbits
Default_Handler:
Infinite_Loop:
  b Infinite_Loop
  .size Default_Handler, .-Default_Handler

  .section .isr_vector,"a",%progbits
  .type g_pfnVectors, %object
  .size g_pfnVectors, .-g_pfnVectors
g_pfnVectors:
  .word _estack
  .word Reset_Handler
  .word NMI_Handler
  .word HardFault_Handler
  .word MemManage_Handler
  .word BusFault_Handler
  .word UsageFault_Handler
  .word 0
  .word 0
  .word 0
  .word 0
  .word SVC_Handler
  .word DebugMon_Handler
  .word 0
  .word PendSV_Handler
  .word SysTick_Handler

  .weak NMI_Handler
  .thumb_set NMI_Handler,Default_Handler
  .weak HardFault_Handler
  .thumb_set HardFault_Handler,Default_Handler
  .weak MemManage_Handler
  .thumb_set MemManage_Handler,Default_Handler
  .weak BusFault_Handler
  .thumb_set BusFault_Handler,Default_Handler
  .weak UsageFault_Handler
  .thumb_set UsageFault_Handler,Default_Handler
  .weak SVC_Handler
  .thumb_set SVC_Handler,Default_Handler
  .weak DebugMon_Handler
  .thumb_set DebugMon_Handler,Default_Handler
  .weak PendSV_Handler
  .thumb_set PendSV_Handler,Default_Handler
  .weak SysTick_Handler
  .thumb_set SysTick_Handler,Default_Handler
