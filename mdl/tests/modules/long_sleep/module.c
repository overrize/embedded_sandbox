/*
 * H2 test: sleeping is not hanging.
 *
 * Sleeps for twice the watchdog timeout in one call, then says so. If the
 * watchdog cannot tell "blocked in the host" from "stuck", this MDL is
 * killed part way through and the second line never appears.
 *
 * This was a real bug before ABI v3, not a hypothetical: delay_ms() fed
 * the watchdog on the way IN and then blocked, so any sleep longer than
 * MDL_WATCHDOG_TIMEOUT_MS killed the module that asked for it. Nothing
 * caught it because every MDL written so far slept in small increments.
 *
 * Expected on the console:
 *   [mdl] long_sleep: sleeping 6000ms in one call (watchdog timeout is 3000)
 *   [mdl] long_sleep: woke up -- sleeping is not hanging
 * and `status` still RUNNING/LOADED, never FAULTED.
 */
#include "host_api.h"

MDL_MODULE_ABI_DECLARE();
MDL_MODULE_RESOURCES();  /* touches no hardware at all */

int module_init(const host_api_t *host)
{
    host->log("long_sleep: sleeping 6000ms in one call (watchdog timeout is 3000)");
    host->delay_ms(6000);
    host->log("long_sleep: woke up -- sleeping is not hanging");

    /* Now the other half of the contract: a long stretch of real work with
     * no sleeping in it. This has to feed explicitly or the host is right
     * to kill it. */
    host->log("long_sleep: now 5s of 'work', feeding explicitly");
    for (int i = 0; i < 50; i++) {
        volatile uint32_t spin = 200000;
        while (spin--) {
        }
        host->watchdog_feed();
    }
    host->log("long_sleep: survived the working stretch too");
    return 0;
}
