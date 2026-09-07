/*
 * Module 3 of 3: hold the button on whitelist pin 2 -> the GREEN led BLINKS.
 *
 * Deliberately the same input as ../sw3_blue with a different output and
 * a different behaviour, so loading one after the other shows the same
 * physical button doing something else entirely -- which is the point of
 * the whole project, stated in one gesture.
 *
 * Also the only one of the three whose loop has state: it has to keep
 * track of where it is in the blink cycle, and that state lives in the
 * module's own data region.
 */
#include "host_api.h"

MDL_MODULE_ABI_DECLARE();
MDL_MODULE_RESOURCES(MDL_RES_GPIO(1),   /* LEDG, green */
                     MDL_RES_GPIO(2));  /* BTN0        */

/* Non-zero initialiser, so this lands in real .data and is visibly
 * relocated in a `mem 0x20024000` dump rather than being indistinguishable
 * from zeroed .bss. */
static int g_phase = 1;

int module_init(const host_api_t *host)
{
    host->log("sw3_blink resident: hold the button, green led blinks");

    for (;;) {
        if (host->gpio_get(2) == 0) {   /* pressed */
            g_phase = !g_phase;
            host->gpio_set(1, g_phase); /* 0 = lit */
            host->delay_ms(120);
        } else {
            host->gpio_set(1, 1);       /* released: off */
            g_phase = 1;
            host->delay_ms(20);
        }
    }
    return 0; /* not reached */
}
