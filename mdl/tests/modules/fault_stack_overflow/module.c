/*
 * Negative test 4/6: recurse until the module's own PSP stack (2K,
 * mdl/core/module_task.h) runs into the unmapped 2K guard gap below it.
 * Expect: MemManage fault, not silent corruption of the heap pool that
 * sits below the guard.
 */
#include "host_api.h"

MDL_MODULE_ABI_DECLARE();

static void __attribute__((noinline)) recurse(const host_api_t *host, volatile int depth)
{
    volatile uint8_t eat_stack[256];
    eat_stack[0] = (uint8_t)depth;
    eat_stack[255] = (uint8_t)depth;
    (void)host;
    recurse(host, depth + 1); /* should never return -- ~8 levels (2K / 256B) exhausts the stack block */
}

int module_init(const host_api_t *host)
{
    host->log("fault_stack_overflow: recursing until the guard gap catches it");
    recurse(host, 0);
    host->log("fault_stack_overflow: SHOULD NOT REACH HERE");
    return -1;
}
