/*
 * The same behaviour as ../sw3_blue -- hold SW3, the blue LED follows --
 * arrived at the other way, and the difference is the point of ABI v4.
 *
 *   sw3_blue   module_init() never returns; it polls the pin at 50Hz and
 *              owns the module task forever.
 *   sw3_irq    module_init() RETURNS. The pin's interrupt wakes the MDL's
 *              resident task, which calls module_event(). Nothing polls,
 *              and the task is free between edges -- which is what makes
 *              it able to answer a console command as well.
 *
 * The interrupt itself is host code: an ISR runs privileged, so an MDL
 * handler called from one would run privileged too and the sandbox would
 * be worth nothing. The host ISR captures the edge and wakes this task;
 * module_event() runs here, unprivileged, one context switch later.
 */
#include "host_api.h"

MDL_MODULE_ABI_DECLARE();

/* Four slots is plenty for a button: the host merges same-pin events when
 * they outrun us, and 50/s is far above what a finger produces -- so if
 * the host ever reports exceeding it, that is contact bounce worth
 * knowing about rather than a number to quietly raise. */
MDL_MODULE_EVENTS(4, 50);

MDL_MODULE_RESOURCES(MDL_RES_GPIO(0),                      /* LEDB, blue */
                     MDL_RES_GPIO_IRQ(2, MDL_EDGE_BOTH));  /* SW3, both edges */

static int g_edges = 1;   /* non-zero initialiser -> real .data */

int module_init(const host_api_t *host)
{
    host->log("sw3_irq: interrupt-driven, press SW3 (module_init returns here)");
    host->gpio_set(0, 1); /* active-low: 1 = off */
    return 0;
}

int module_event(const host_api_t *host, const mdl_event_t *evt)
{
    if (evt->source != MDL_EVT_GPIO || evt->id != 2) {
        return 0;
    }

    /* payload is the pin level captured in the ISR. Button and LED are
     * both active-low on this board, so the level IS the LED state. */
    host->gpio_set(0, (int)evt->payload);

    /* coalesced > 1 means edges arrived faster than this task drained
     * them and the host merged them. For a lamp that is invisible and
     * fine; the count is here so a handler that cares about EDGES rather
     * than STATE can still get the number right. */
    g_edges += (int)evt->coalesced;
    return 0;
}
