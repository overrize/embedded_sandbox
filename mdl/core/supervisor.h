#ifndef MDL_SUPERVISOR_H
#define MDL_SUPERVISOR_H

#include "registry.h"

/* NOT #include "host_api.h": nothing declared in this header mentions a
 * host_api type. It used to be included here, which quietly made every
 * translation unit that wants mdl_supervisor_wake_from_isr() -- notably
 * mdl/arch/arm_cm4/fault_arm.c, the arch layer -- need mdl/host on its
 * include path. That broke the bare-metal M0/M1 builds the moment M3
 * added this include to fault_arm.c, and went unnoticed because neither
 * was rebuilt afterwards. supervisor.c includes host_api.h itself, where
 * the dependency is real. */

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

/*
 * ISR-context only, generic: wakes the supervisor task (a task
 * notification) and requests a context switch on exception/interrupt
 * return via portYIELD_FROM_ISR. Two callers: mdl/arch/arm_cm4/
 * fault_arm.c (right after mdl_record_fault() classifies a fault as
 * module-internal) and mdl/transport/protocol.c (when a complete,
 * checksum-valid frame lands). Both are safe to call from here only
 * because their respective NVIC priorities are explicitly set at or
 * below configMAX_SYSCALL_INTERRUPT_PRIORITY urgency (see main.c's boot
 * init for MemManage; the USB IRQ needs the same treatment -- see
 * mdl/transport/usb_cdc.c). The supervisor doesn't need to know *why*
 * it woke -- mdl_supervisor_run()'s loop just re-checks everything
 * (fault/watchdog state, then a pending protocol frame) on every wake,
 * whichever source it came from, or on the plain poll-interval timeout. */
void mdl_supervisor_wake_from_isr(void);

/*
 * The supervisor task's main loop body -- never returns. Blocks on the
 * wake notification with a timeout (MDL_WATCHDOG_POLL_MS); on every
 * wake, whatever woke it:
 *   1. MDL_SLOT_FAULTED (a fault handler already classified and marked
 *      it): reclaim now.
 *   2. MDL_SLOT_RUNNING and last_active_tick is older than
 *      MDL_WATCHDOG_TIMEOUT_MS: presumed hung (the "while(1){}" case) --
 *      mark faulted and reclaim.
 *   3. A protocol frame is pending (mdl_proto_take_frame()): handle the
 *      load/unload/status request, respond over the transport.
 * Reclaim = vTaskDelete() the module's task (never done from the fault
 * ISR itself, per the spec's hard rule) + reset all host-side
 * bookkeeping (alloc pool, registry state back to MDL_SLOT_EMPTY) so
 * the slot is immediately ready for the next mdl_load().
 */
void mdl_supervisor_run(void) __attribute__((noreturn));

/*
 * Unload the module and reclaim everything it held (task, alloc pool,
 * registry state), the same way MDL_CMD_UNLOAD does -- but without
 * writing a binary protocol response, since the caller is the text
 * console rather than a framed request.
 *
 * SUPERVISOR-TASK CONTEXT ONLY. mdl/transport/console.c satisfies that
 * because mdl_console_execute() is called from mdl_supervisor_run()'s
 * own loop, not from the USB RX task that assembled the line. Calling
 * this from anywhere else races the supervisor's fault/watchdog handling
 * over the same slot.
 */
void mdl_supervisor_request_unload(void);

#endif /* MDL_SUPERVISOR_H */
