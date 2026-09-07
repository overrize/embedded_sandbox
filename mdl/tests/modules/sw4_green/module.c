/*
 * Module 2 of 3: hold the button on whitelist pin 3 -> the GREEN led lights.
 *
 * Same shape as ../sw3_blue, different pins. Pushing this one after that
 * one is the actual demonstration: the board's behaviour changes with no
 * reflash and no reset, and `unload` puts it back.
 *
 * Claims LEDG (pin 1), which the host uses for its USB-link indicator --
 * declared, so the host yields it.
 */
#include "host_api.h"

MDL_MODULE_ABI_DECLARE();
MDL_MODULE_RESOURCES(MDL_RES_GPIO(1),   /* LEDG, green */
                     MDL_RES_GPIO(3));  /* BTN1        */

int module_init(const host_api_t *host)
{
    host->log("sw4_green resident: hold the other button, green led follows");

    for (;;) {
        host->gpio_set(1, host->gpio_get(3));
        host->delay_ms(20);
    }
    return 0; /* not reached */
}
