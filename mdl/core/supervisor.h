#ifndef MDL_SUPERVISOR_H
#define MDL_SUPERVISOR_H

#include "registry.h"
#include "host_api.h"

/*
 * The loader/supervisor task's fault-recovery and watchdog machinery.
 * "加载/卸载操作本身在独立的 loader 任务里做,不能在中断里做" -- this is
 * that task's body; the ISR (mdl/arch/arm_cm4/fault_arm.c) only ever
 * classifies and *wakes* this task, never touches FreeRTOS task/queue
 * state itself.
 */

#define MDL_WATCHDOG_TIMEOUT_MS 3000u  /* no host call in this long while
                                         * RUNNING -> presumed hung */
#define MDL_WATCHDOG_POLL_MS     500u  /* worst-case extra latency before
                                         * a hang is noticed, on top of
                                         * the timeout itself */

/* Call once, from the supervisor/loader task itself (so it captures the
 * right TaskHandle_t), before the task's main loop starts. */
void mdl_supervisor_init(void);

/* ISR-context only: mdl/arch/arm_cm4/fault_arm.c calls this right after
 * mdl_record_fault() classifies a fault as module-internal. Wakes the
 * supervisor task (a task notification) and requests a context switch
 * on exception return via portYIELD_FROM_ISR -- safe to call here only
 * because MemManage_Handler's priority is explicitly set at or below
 * configMAX_SYSCALL_INTERRUPT_PRIORITY urgency (see main.c's boot init;
 * "at or below" in ARM's inverted numeric sense, i.e. numerically >=). */
void mdl_supervisor_notify_fault_from_isr(void);

/*
 * The supervisor task's main loop body -- never returns. Blocks on the
 * fault notification with a timeout; on each wake (fault or timeout)
 * checks the module's state:
 *   - MDL_SLOT_FAULTED (a fault handler already classified and marked
 *     it): reclaim now.
 *   - MDL_SLOT_RUNNING and last_active_tick is older than
 *     MDL_WATCHDOG_TIMEOUT_MS: presumed hung (the "while(1){}" case) --
 *     mark faulted and reclaim.
 *   - anything else: nothing to do, loop again.
 * Reclaim = vTaskDelete() the module's task (never done from the fault
 * ISR itself, per the spec's hard rule) + reset all host-side
 * bookkeeping (alloc pool, registry state back to MDL_SLOT_EMPTY) so
 * the slot is immediately ready for the next mdl_load().
 */
void mdl_supervisor_run(void) __attribute__((noreturn));

#endif /* MDL_SUPERVISOR_H */
