#include "arch_if.h"
#include "registry.h" /* mdl_record_fault() -- plain integers in, no CMSIS out */
#include "supervisor.h" /* mdl_supervisor_wake_from_isr() -- ISR-safe, see its own doc */
#include "cmsis_device.h" /* per-project device header indirection -- see mpu_armv7m.c's comment */

bool arch_pc_in_range(uintptr_t pc, uintptr_t lo, uintptr_t hi)
{
    return pc >= lo && pc < hi;
}

/*
 * Weak hook: give the fault somewhere to survive the reset below.
 *
 * Both arch_system_reset() calls in this file are unrecoverable host
 * faults, and both used to be completely silent -- the reset wipes .bss,
 * so mdl_record_fault()'s copy is gone by the time anyone can ask, and
 * board_debug.c never sees these at all because MemManage is owned here,
 * not there. The result was a device that rebooted with no evidence, for
 * every host-side MemManage. A board layer with somewhere persistent to
 * put it (mdl/tests/hil/common/board_debug.c uses a .noinit record)
 * overrides this; targets without one keep the no-op and behave exactly
 * as before.
 *
 * `stacked` is NULL when CFSR.MSTKERR says the exception frame was never
 * written -- an implementation must not dereference it in that case.
 */
__attribute__((weak)) void arch_fault_persist(const char *which, uint32_t *stacked,
                                              uint32_t cfsr, uint32_t mmfar)
{
    (void)which;
    (void)stacked;
    (void)cfsr;
    (void)mmfar;
}
/* Not one of arch_if.h's five required functions -- an extra safety
 * primitive the fault handler needs for host-level (unrecoverable) faults. */
void arch_system_reset(void)
{
    NVIC_SystemReset();
}

/*
 * Where a faulted module task's saved PC gets redirected to instead of
 * resuming whatever it was doing. Plain infinite loop, in ordinary
 * (unprivileged-executable) flash -- NOT tagged privileged_functions,
 * or the module would just re-fault immediately trying to fetch from
 * here. Runs at the module task's own (lowest) priority, so it costs
 * nothing but a little idle time before mdl_supervisor_run() gets
 * around to vTaskDelete()-ing it -- typically near-instant, since
 * notifying the supervisor also requests an immediate context switch.
 */
__attribute__((used, noinline))
static void mdl_module_trap(void)
{
    for (;;) {
        __NOP();
    }
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
         * dereferenced. CFSR still says which fault it was, so persist
         * that much before resetting -- MSTKERR on its own already names
         * the failure (the stack the exception tried to push onto was not
         * writable), which is most of the answer. */
        arch_fault_persist("MemManage MSTKERR (no usable frame)", 0, cfsr, 0xFFFFFFFFu);
        arch_system_reset();
        return;
    }

    uint32_t mmfar = (cfsr & SCB_CFSR_MMARVALID_Msk) ? SCB->MMFAR : 0;
    uint32_t pc = stacked[6]; /* hardware exception frame: r0 r1 r2 r3 r12 lr pc xpsr */
    uint32_t lr = stacked[5];

    bool is_module_fault = mdl_record_fault(pc, lr, mmfar, cfsr);

    /* CFSR fault-status bits are write-1-to-clear. */
    SCB->CFSR = cfsr;

    if (!is_module_fault) {
        /* Fault PC wasn't inside the loaded module's text -- a host-side
         * bug (or no module loaded at all). Not recoverable the way a
         * module fault is: the host's own state (kernel data structures,
         * peripheral drivers mid-operation, etc.) could be in any state.
         * "USB CDC 和 loader 任务在任何情况下都要活着" is a promise about
         * surviving MODULE crashes, not host bugs -- a host bug gets a
         * clean reset instead of limping on corrupted. */
        /* mmfar was captured above, BEFORE the CFSR write-1-to-clear a few
         * lines up: clearing MMARVALID makes SCB->MMFAR architecturally
         * UNKNOWN, so re-reading the register here would record a value
         * that looks authoritative and is not. */
        arch_fault_persist("MemManage in HOST code (pc outside module text)",
                           stacked, cfsr, mmfar);
        arch_system_reset();
        return;
    }

    /*
     * Redirect the faulted task's saved PC to the trap loop instead of
     * resuming the faulting instruction (which would just fault again,
     * in a tight loop, forever). thumb bit (bit 0) set, matching every
     * other Thumb code pointer on this target -- see loader.c's
     * init_off comment for why that bit matters to BLX/BX. Do NOT
     * delete the task here -- mdl_supervisor_wake_from_isr() only wakes
     * the supervisor task; the actual vTaskDelete() happens from ITS
     * context (mdl/core/supervisor.c), per the spec's explicit "不在
     * handler 里删任务" rule.
     */
    stacked[6] = ((uint32_t)mdl_module_trap) | 1u;

    mdl_supervisor_wake_from_isr();
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
