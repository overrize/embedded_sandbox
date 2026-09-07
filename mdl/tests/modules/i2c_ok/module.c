/*
 * R3 positive case: I2C1 is free on this board, so the claim is granted.
 *
 * PB6/PB7 are not held by the host and not used by anything else this MDL
 * asks for, so the load succeeds and `pins`/`status` show the claim.
 *
 * It cannot USE the bus: declaring a peripheral reserves it and gets it
 * checked for conflicts, but the vtable has no I2C operations yet. The
 * arbitration mechanism landed before the drivers on purpose -- getting
 * ownership wrong is the expensive mistake, and it is the one that has to
 * be right before several peripherals are in play.
 */
#include "host_api.h"

MDL_MODULE_ABI_DECLARE();
MDL_MODULE_RESOURCES(MDL_RES_I2C(1),      /* PB6 SCL, PB7 SDA -- free */
                     MDL_RES_TIMER(3),    /* PA6/PA7/PB0/PB1 -- free  */
                     MDL_RES_GPIO(1));    /* LEDG, host yields it     */

int module_init(const host_api_t *host)
{
    host->log("i2c_ok: claimed I2C1 + TMR3 + LEDG, no conflicts");
    host->gpio_set(1, 0);   /* prove the GPIO half still works */
    host->delay_ms(300);
    host->gpio_set(1, 1);
    return 0;
}
