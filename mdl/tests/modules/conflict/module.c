/*
 * The negative half of the resource test: this module is SUPPOSED to be
 * refused.
 *
 * It claims GPIO 4, which is PA9 -- the DAP debug UART's TX line. That
 * pin is on the whitelist for exactly one reason: so a module asking for
 * it can be turned down by name. Calling gpio_init() on it reconfigures
 * the line the debug console is speaking over, so the failure would take
 * away the very channel you would use to find out what happened.
 *
 * This is the HARD half of the ownership model. Contrast ../sw3_blue,
 * which claims LEDB -- also in use by the host, but as a courtesy the
 * host gives up on request. The distinction is whether the host can still
 * be a working host without the pin.
 *
 * Expected result, with no byte of it ever reaching the arena:
 *
 *   -> RESP_ERROR  resource conflict: gpio 4 is owned by
 *                  the DAP debug UART (PA9/PA10)
 *
 * Change the claim to MDL_RES_GPIO(1) and the identical module loads.
 */
#include "host_api.h"

MDL_MODULE_ABI_DECLARE();
MDL_MODULE_COMMAND("hijack");
MDL_MODULE_RESOURCES(MDL_RES_GPIO(4));

int module_init(const host_api_t *host)
{
    host->log("if you are reading this, the conflict check did not run");
    return 0;
}

int module_cmd(const host_api_t *host, int argc, const char *const *argv)
{
    (void)argc;
    (void)argv;
    /* Never reached: the load is refused before any of this is copied in. */
    host->gpio_set(0, 0);
    return 0;
}
