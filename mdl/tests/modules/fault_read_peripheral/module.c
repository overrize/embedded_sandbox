/*
 * Negative test 2/6: read a peripheral register directly, bypassing the
 * gpio_get() vtable call entirely. Expect: MemManage fault (peripherals
 * are granted only to privileged code -- portGENERAL_PERIPHERALS_REGION
 * is region 3, never one of this task's 3 configurable regions).
 */
#include "host_api.h"

MDL_MODULE_ABI_DECLARE();

int module_init(const host_api_t *host)
{
    host->log("fault_read_peripheral: about to read 0x40000000");
    volatile uint32_t *gpioa_base = (volatile uint32_t *)0x40000000u;
    uint32_t v = *gpioa_base; /* should never return */
    host->log("fault_read_peripheral: SHOULD NOT REACH HERE");
    return (int)v;
}
