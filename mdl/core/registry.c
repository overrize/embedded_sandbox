#include "registry.h"
#include "arch_if.h"
#include <stddef.h>

module_t g_mdl_slot;
mdl_fault_info_t g_mdl_last_fault;

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
