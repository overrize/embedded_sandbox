/*
 * Negative test 5/6: hang with no host interaction at all -- must NOT
 * call host->log()/delay_ms()/anything, or that call would feed the
 * software watchdog (mdl/host/host_api.c's feed_watchdog(), called by
 * every SVC-gated function) and this would stop being the test it's
 * supposed to be. Expect: no MemManage fault ever (this is legal code,
 * just legal code that never yields) -- mdl_supervisor_run()'s
 * MDL_WATCHDOG_TIMEOUT_MS poll notices last_active_tick hasn't moved
 * and kills the task itself.
 */
#include "host_api.h"

MDL_MODULE_ABI_DECLARE();

int module_init(const host_api_t *host)
{
    (void)host; /* deliberately never called below */
    for (;;) {
        /* nothing -- no host call, no yield */
    }
}
