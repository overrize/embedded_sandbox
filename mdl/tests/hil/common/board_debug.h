#ifndef MDL_BOARD_DEBUG_H
#define MDL_BOARD_DEBUG_H

#include <stdint.h>

/*
 * Fault capture for hardware bring-up.
 *
 * WHY THIS EXISTS: the first M4 run on real silicon stopped with
 *
 *     HFSR = 0x40000000  (FORCED)
 *     CFSR = 0x00000400  (BFSR bit 2, IMPRECISERR)
 *     BFARVALID = 0, PC reported as vListInsertEnd
 *
 * "Imprecise" is the whole problem: the Cortex-M4's write buffer lets a
 * store retire before the bus transaction completes, so the fault is
 * raised at whatever instruction happens to be executing when the
 * transaction finally fails -- NOT at the store that caused it. The
 * reported PC is therefore meaningless, and BFAR is not even populated.
 * Chasing that PC is chasing noise.
 *
 * board_debug_faults_init() fixes both halves of that:
 *   1. Sets ACTLR.DISDEFWBUF, which disables the write buffer. Every
 *      store then completes in order, so a faulting store raises a
 *      PRECISE bus fault at the offending instruction with BFAR valid.
 *   2. Enables the BusFault, MemManage and UsageFault exceptions, so
 *      they are taken by their own handlers with their own status bits
 *      intact instead of escalating into a HardFault that has already
 *      lost the detail (which is what FORCED=1 above means).
 *
 * Cost: DISDEFWBUF makes stores slower, because that is exactly what a
 * write buffer is for. It is a bring-up tool, not a shipping setting --
 * see BOARD_DEBUG_PRECISE_BUS_FAULTS below.
 */

/*
 * Disable the write buffer (ACTLR.DISDEFWBUF) so bus faults are precise.
 *
 * DEFAULT 0 SINCE 2026-09-07. It was 1 through bring-up and earned its
 * keep -- both the .privileged_data fault and the MPU-attribute one were
 * found with it. But it makes EVERY store in the system slower, and this
 * project's central claim is that a loaded MDL costs almost nothing
 * against an interpreter. Leaving a bring-up switch on would make every
 * number measured here wrong in our own favour, which is the worst
 * direction for a benchmark to be wrong in.
 *
 * Turn it back on for a session chasing a bus fault:
 *   make PRECISE_FAULTS=1        (or -DBOARD_DEBUG_PRECISE_BUS_FAULTS=1)
 * The console's `ver` reports which way this build was compiled, so a
 * measurement cannot be taken on a slowed image by accident.
 */
#ifndef BOARD_DEBUG_PRECISE_BUS_FAULTS
#define BOARD_DEBUG_PRECISE_BUS_FAULTS 0
#endif

/*
 * Everything a fault handler could recover, in one struct so a debugger
 * only needs one watch expression. `which` names the exception, so a
 * HardFault escalation is distinguishable from a real BusFault at a
 * glance.
 */
typedef struct {
    uint32_t magic;   /* 0xFA017ED0 once written -- distinguishes a real
                        * record from never-faulted zeroed .bss */
    const char *which;

    /* Hardware-stacked exception frame */
    uint32_t r0, r1, r2, r3, r12, lr, pc, xpsr;

    /* Fault status */
    uint32_t cfsr;   /* MMFSR | BFSR | UFSR, all three packed */
    uint32_t hfsr;
    uint32_t bfar;   /* valid only if CFSR bit 15 (BFARVALID) */
    uint32_t mmfar;  /* valid only if CFSR bit 7  (MMARVALID) */
    uint32_t shcsr;
    uint32_t exc_return; /* EXC_RETURN: bit 2 says which stack was in use */
} board_fault_record_t;

#define BOARD_FAULT_MAGIC 0xFA017ED0u

/* Watch this one symbol in Ozone after a stop. */
extern volatile board_fault_record_t g_board_fault;

/*
 * Call FIRST in main(), before board_clock_init() -- an imprecise fault
 * from the clock setup itself would otherwise still be imprecise.
 */
void board_debug_faults_init(void);

/*
 * Print the record left by the PREVIOUS run, if that run faulted.
 *
 * Takes the output function rather than calling the console directly so
 * this file keeps its only dependency on the CMSIS headers: M0 and M1
 * link board_debug.c but have no console at all, and would not link if
 * mdl_console_puts() were referenced here. Pass mdl_console_puts.
 *
 * Prints nothing but a "clean" line when g_board_fault.magic does not
 * match -- which is the normal case, and also what an uninitialised
 * .noinit looks like on the very first power-up.
 */
void board_debug_print_fault(void (*out)(const char *));

#endif /* MDL_BOARD_DEBUG_H */
