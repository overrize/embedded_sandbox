#include "registry.h"
#include <stddef.h>

module_t g_mdl_slot;
mdl_fault_info_t g_mdl_last_fault;

void registry_init(void)
{
    g_mdl_slot.state = MDL_SLOT_EMPTY;
    g_mdl_slot.entry = NULL;
    g_mdl_last_fault.occurred = false;
}

void mdl_record_fault(uint32_t pc, uint32_t lr, uint32_t mmfar, uint32_t cfsr)
{
    g_mdl_last_fault.occurred = true;
    g_mdl_last_fault.pc = pc;
    g_mdl_last_fault.lr = lr;
    g_mdl_last_fault.mmfar = mmfar;
    g_mdl_last_fault.cfsr = cfsr;

    if (g_mdl_slot.state != MDL_SLOT_EMPTY) {
        g_mdl_slot.state = MDL_SLOT_FAULTED;
    }

    /*
     * M3 adds: arch_pc_in_range() classification against
     * g_mdl_slot.text_lo/text_hi to tell "module fault" from "host bug",
     * marking the slot and returning control to the loader task instead
     * of halting here, and computing offset-from-text-start so the PC
     * side can addr2line it. This function is the seam that work lands
     * on -- its signature and callers should not need to change for it.
     */
}
