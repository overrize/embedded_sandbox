#include "arch_if.h"
#include "registry.h" /* mdl_record_fault() -- plain integers in, no CMSIS out */
#include "at32f435_437.h" /* device header first -- see the comment in mpu_armv7m.c */

bool arch_pc_in_range(uintptr_t pc, uintptr_t lo, uintptr_t hi)
{
    return pc >= lo && pc < hi;
}

/* Not one of arch_if.h's five required functions -- an extra safety
 * primitive the fault handler needs for host-level (unrecoverable) faults. */
void arch_system_reset(void)
{
    NVIC_SystemReset();
}

/* used: this is reachable only through the inline-asm branch in
 * MemManage_Handler below, which GCC's dead-code elimination cannot see
 * -- without this attribute an unoptimized-looking "unused static
 * function" gets deleted entirely (not just its section dropped by
 * --gc-sections), producing a link-time undefined reference. */
__attribute__((used))
static void mdl_memmanage_handler_c(uint32_t *stacked)
{
    uint32_t cfsr = SCB->CFSR;

    if (cfsr & SCB_CFSR_MSTKERR_Msk) {
        /* Exception entry's own stacking failed -- CFSR.MSTKERR being set
         * means the frame at `stacked` was never written and must not be
         * dereferenced. Nothing left here to record; go straight to a
         * safe reset rather than read garbage. */
        arch_system_reset();
        return;
    }

    uint32_t mmfar = (cfsr & SCB_CFSR_MMARVALID_Msk) ? SCB->MMFAR : 0;
    uint32_t pc = stacked[6]; /* hardware exception frame: r0 r1 r2 r3 r12 lr pc xpsr */
    uint32_t lr = stacked[5];

    mdl_record_fault(pc, lr, mmfar, cfsr);

    /* CFSR fault-status bits are write-1-to-clear. */
    SCB->CFSR = cfsr;

    /*
     * M0: nothing runs unprivileged yet, and there is no loader task to
     * hand this off to -- that dispatch (arch_pc_in_range() against
     * g_mdl_slot's text bounds, mark-and-return-to-the-loader-task
     * instead of the faulted context) lands in M3 once modules actually
     * execute unprivileged. For now: halt so the self-test can inspect
     * g_mdl_last_fault post-mortem, instead of resuming into whatever
     * corrupted state caused the fault.
     */
    for (;;) {
        __NOP();
    }
}

__attribute__((naked)) void MemManage_Handler(void)
{
    __asm volatile (
        "tst   lr, #4                    \n"
        "ite   eq                        \n"
        "mrseq r0, msp                   \n"
        "mrsne r0, psp                   \n"
        "b     mdl_memmanage_handler_c   \n"
    );
}
