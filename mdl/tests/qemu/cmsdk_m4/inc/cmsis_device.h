/*
 * QEMU `-M mps2-an386` target's cmsis_device.h -- see the comment in
 * mdl/arch/arm_cm4/mpu_armv7m.c for what this indirection is for. No
 * vendor device header exists for a plain QEMU CMSDK target, so this
 * file defines the handful of macros CMSIS-Core's core_cm4.h needs
 * directly, then includes it. core_cm4.h itself is NOT vendored here --
 * it's the same file the AT32 targets use (CMSIS-Core is a standard,
 * vendor-independent file; only the *device* header differs), reached
 * via this project's own -I onto the vendor BSP's copy (see the
 * Makefile) purely because a spare copy happens to already be on disk
 * in this sandbox -- point it at any real CMSIS-Core distribution's
 * core_cm4.h and it works identically.
 */
#ifndef __CM4_REV
#define __CM4_REV       0x0001U
#define __MPU_PRESENT   1
#define __NVIC_PRIO_BITS 8      /* Measured empirically against the actual QEMU binary
                                  * installed in this sandbox, not assumed from generic
                                  * CMSDK documentation (an earlier guess of 3 bits was
                                  * WRONG and caught immediately by FreeRTOS-MPU's own
                                  * boot-time configASSERT in xPortStartScheduler(),
                                  * which empirically probes the same register this
                                  * comment is about): write 0xFF to the first user
                                  * interrupt's priority register at 0xE000E400 and read
                                  * it back -- this QEMU build's mps2-an386 model returns
                                  * 0xFF (all 8 bits stick), i.e. 8 priority bits, not 3.
                                  * If you rebuild against a different QEMU version and
                                  * boot hits the same assert again, re-run that probe
                                  * rather than re-guessing. */
#define __Vendor_SysTickConfig 0
#define __FPU_PRESENT   1U
#endif

typedef enum IRQn {
    NonMaskableInt_IRQn   = -14,
    HardFault_IRQn        = -13,
    MemoryManagement_IRQn = -12,
    BusFault_IRQn         = -11,
    UsageFault_IRQn       = -10,
    SVCall_IRQn           = -5,
    DebugMonitor_IRQn     = -4,
    PendSV_IRQn           = -2,
    SysTick_IRQn          = -1,

    /* mps2-an386 has real UART/timer/etc IRQ lines too, but the M0
     * self-test (and everything else this target currently runs) uses
     * none of them -- add entries here if/when a QEMU-side test needs
     * one. */
} IRQn_Type;

#include "core_cm4.h"
