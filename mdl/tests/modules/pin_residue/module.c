/*
 * Test for R1: does an unloaded MDL leave its pins the way it found them?
 *
 * It claims the green LED and turns it ON, then does nothing forever. The
 * check is what happens at `unload`:
 *
 *   with the fix    green goes out   -- the host took the pin back and
 *                                       put it in its default state
 *   without it      green stays lit  -- nothing ever undid the MDL's last
 *                                       write, and the next MDL inherits
 *                                       a pin already doing something
 *
 * That residue is invisible with the demo MDLs (sw3_blue and friends leave
 * their LED off anyway, and the host reclaims its indicators), which is
 * exactly why it needed a test that makes the leftover state impossible to
 * miss.
 */
#include "host_api.h"

MDL_MODULE_ABI_DECLARE();
MDL_MODULE_RESOURCES(MDL_RES_GPIO(1));  /* LEDG */

int module_init(const host_api_t *host)
{
    host->log("pin_residue: green led ON and left that way -- now unload me");
    host->gpio_set(1, 0);  /* active-low: 0 = lit */

    for (;;) {
        /* Idle, but through a host call, so the software watchdog keeps
         * seeing this MDL as alive rather than hung. */
        host->delay_ms(200);
    }
    return 0; /* not reached */
}
