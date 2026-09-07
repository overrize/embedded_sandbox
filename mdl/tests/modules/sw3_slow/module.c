/*
 * Exercises the coalescing path, which ../sw3_irq never reaches because a
 * finger cannot outrun the module task.
 *
 * module_event() sleeps 400ms per event on purpose. Press SW3 several
 * times quickly and edges arrive faster than they are drained, so the
 * host merges same-pin events instead of dropping them -- and reports how
 * many it merged, which is the whole argument for carrying the count:
 *
 *   [mdl] evt level=1 coalesced=1     drained faster than they arrived
 *   [mdl] evt level=0 coalesced=6     six edges folded into this one
 *
 * The LED follows the LATEST level, which is what merging is supposed to
 * preserve. If you were counting edges instead, `coalesced` is the number
 * you would add -- ignoring it is exactly how merging turns into silent
 * data loss for a counter or an encoder.
 *
 * Declares a rate of 2/s deliberately, far below what pressing produces,
 * so `status` also demonstrates the declared-vs-actual report:
 *   declared 2/s, peak seen 11/s  <- OVER DECLARED RATE
 */
#include "host_api.h"

MDL_MODULE_ABI_DECLARE();
MDL_MODULE_EVENTS(2, 2);   /* tiny queue, honest-but-low rate claim */
MDL_MODULE_RESOURCES(MDL_RES_GPIO(0),
                     MDL_RES_GPIO_IRQ(2, MDL_EDGE_BOTH));

static char g_msg[40] = "evt level=? coalesced=?";

int module_init(const host_api_t *host)
{
    host->log("sw3_slow: 400ms per event on purpose -- press SW3 fast");
    host->gpio_set(0, 1);
    return 0;
}

int module_event(const host_api_t *host, const mdl_event_t *evt)
{
    if (evt->source != MDL_EVT_GPIO) {
        return 0;
    }

    host->gpio_set(0, (int)evt->payload);   /* latest level wins */

    g_msg[10] = (char)('0' + (int)(evt->payload & 1u));
    /* Single digit is enough to see merging happen; a real handler would
     * use the number rather than print it. */
    g_msg[22] = (evt->coalesced > 9u) ? '+' : (char)('0' + (int)evt->coalesced);
    host->log(g_msg);

    host->delay_ms(400);    /* the whole point: drain slower than input */
    return 0;
}
