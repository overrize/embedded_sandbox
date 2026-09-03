/*
 * Negative test 3/6: classic null pointer dereference. Expect: MemManage
 * fault (address 0 is never inside any granted region -- the guard
 * region's whole purpose overlaps with this, but 0 isn't even near the
 * arena, so this is really just "address 0 has no region" more broadly).
 */
#include "host_api.h"

MDL_MODULE_ABI_DECLARE();

int module_init(const host_api_t *host)
{
    host->log("fault_null_deref: about to deref NULL");
    volatile int *p = (volatile int *)0;
    int v = *p; /* should never return */
    host->log("fault_null_deref: SHOULD NOT REACH HERE");
    return v;
}
