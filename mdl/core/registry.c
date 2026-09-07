#include "registry.h"
#include "host_events.h"
#include "arch_if.h"
#include <stddef.h>

module_t g_mdl_slot;
mdl_fault_info_t g_mdl_last_fault;

/*
 * Weak no-op default for the arch layer's fault path.
 *
 * mdl/arch/arm_cm4/fault_arm.c calls mdl_supervisor_wake_from_isr() after
 * classifying a fault as module-internal. On the RTOS targets (M2+)
 * mdl/core/supervisor.c provides the real, strong definition and this one
 * is discarded by the linker. On the bare-metal targets (M0/M1) there is
 * no supervisor task to wake -- and no module task either, so the branch
 * that calls it is unreachable -- but the reference still has to resolve
 * at link time. Defining it weakly here, in the same file the arch layer
 * already talks to for fault classification, keeps fault_arm.c free of
 * any build-configuration #ifdef.
 */
__attribute__((weak)) void mdl_supervisor_wake_from_isr(void)
{
}

void registry_init(void)
{
    g_mdl_slot.state = MDL_SLOT_EMPTY;
    g_mdl_slot.entry = NULL;
    g_mdl_slot.task_handle = NULL;
    g_mdl_slot.last_active_tick = 0;
    g_mdl_last_fault.occurred = false;
}

bool mdl_record_fault(uint32_t pc, uint32_t lr, uint32_t mmfar, uint32_t cfsr)
{
    g_mdl_last_fault.occurred = true;
    g_mdl_last_fault.pc = pc;
    g_mdl_last_fault.lr = lr;
    g_mdl_last_fault.mmfar = mmfar;
    g_mdl_last_fault.cfsr = cfsr;

    bool is_module_fault = g_mdl_slot.state != MDL_SLOT_EMPTY &&
                            arch_pc_in_range(pc, g_mdl_slot.text_lo, g_mdl_slot.text_hi);

    g_mdl_last_fault.text_offset = is_module_fault ? (pc - g_mdl_slot.text_lo) : 0xFFFFFFFFu;

    if (is_module_fault) {
        g_mdl_slot.state = MDL_SLOT_FAULTED;
    }

    /* Classification only -- deciding what to DO about it (patch the
     * faulted task's saved PC to a trap, wake the loader task, or reset
     * for an unrecoverable host-side fault) is
     * mdl/arch/arm_cm4/fault_arm.c's job: patching an exception stack
     * frame is ARM-specific stack-layout knowledge this function
     * shouldn't need, and it stays the caller's decision either way. */
    return is_module_fault;
}

/*
 * Weak no-op event layer, so a target without one still links.
 *
 * mdl/host/host_events.c provides the real implementation and any build
 * with a board does. But module_task.c and supervisor.c call these
 * unconditionally now, and M0-M3 predate the event layer entirely -- the
 * exact shape of build rot this project has been bitten by twice (see
 * maintain.md's build discipline note). Same weak-default fix as
 * mdl_supervisor_wake_from_isr() above and mdl_transport_write() in
 * protocol.c: no #ifdefs, no fake source files per target.
 *
 * On such a target no event is ever posted, so take() returning false
 * forever is exactly right -- the resident loop simply waits.
 */
__attribute__((weak)) bool mdl_events_take(mdl_event_t *out)
{
    (void)out;
    return false;
}

__attribute__((weak)) bool mdl_events_post_console(void)
{
    return false;
}

__attribute__((weak)) bool mdl_events_arm_gpio(int pin, uint8_t edge)
{
    (void)pin;
    (void)edge;
    return false;
}

__attribute__((weak)) void mdl_events_get_stats(mdl_events_stats_t *out)
{
    out->delivered = 0;
    out->lost = 0;
    out->coalesced = 0;
    out->declared_hz = 0;
    out->peak_hz = 0;
}

__attribute__((weak)) void mdl_events_disarm_all(void)
{
}

__attribute__((weak)) void mdl_events_reset(uint16_t depth, uint16_t rate_hz,
                                             void *module_task_handle)
{
    (void)depth;
    (void)rate_hz;
    (void)module_task_handle;
}
