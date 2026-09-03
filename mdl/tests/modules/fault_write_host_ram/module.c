/*
 * Negative test 1/6: write straight into host RAM, outside every region
 * this module was ever granted. Expect: MemManage fault, module killed,
 * system survives (mdl/core/supervisor.c reclaims the slot).
 */
#include "host_api.h"

MDL_MODULE_ABI_DECLARE();

int module_init(const host_api_t *host)
{
    host->log("fault_write_host_ram: about to write 0x20000000");
    volatile uint32_t *host_ram_base = (volatile uint32_t *)0x20000000u;
    *host_ram_base = 0xDEADBEEFu; /* should never return */
    host->log("fault_write_host_ram: SHOULD NOT REACH HERE");
    return -1;
}
