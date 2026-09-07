/*
 * Module 1 of 3: hold the button on whitelist pin 2 -> the BLUE led lights.
 *
 * Note what this module does NOT do: return. module_init() runs the
 * behaviour loop itself and stays resident, which is v1's answer to "the
 * device should just keep doing this now". The software watchdog is
 * satisfied because every host call feeds it, and gpio_get()/delay_ms()
 * are host calls. `unload` at the console ends it.
 *
 * It claims LEDB (pin 0), which the host is using for its own 1Hz alive
 * blink. That is allowed *because it is declared*: the host's claim on
 * that pin yields to a module that asks for it, and main.c's
 * indicator_task stops driving it while this module is loaded. An
 * UNdeclared module writing the same pin is the case that stays refused
 * -- see ../conflict/module.c.
 */
#include "host_api.h"

MDL_MODULE_ABI_DECLARE();
MDL_MODULE_RESOURCES(MDL_RES_GPIO(0),   /* LEDB, blue  */
                     MDL_RES_GPIO(2));  /* BTN0        */

int module_init(const host_api_t *host)
{
    host->log("sw3_blue resident: hold the button, blue led follows");

    for (;;) {
        /* Both are active-low and agree by luck of the board: the button
         * reads 0 pressed, and the LED lights when driven 0. So the
         * button level IS the LED level -- no inversion needed, which is
         * a board fact worth stating rather than a coincidence to
         * rediscover later. */
        host->gpio_set(0, host->gpio_get(2));
        host->delay_ms(20); /* 50Hz: far faster than a finger, and it is
                              * what feeds the watchdog */
    }
    return 0; /* not reached */
}
