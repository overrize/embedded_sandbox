/*
 * R3 negative case 1: a claim the host holds HARD.
 *
 * USART1 is PA9/PA10, which is the DAP debug UART -- the channel you would
 * use to find out what went wrong. Taking it away cannot be allowed, and
 * the refusal has to name the pin, because "resource conflict" alone tells
 * the person pushing this nothing they can act on.
 *
 * Expected, with no byte reaching the arena:
 *   -> RESP_ERROR  resource conflict: USART1 (PA9 TX, PA10 RX) needs PA9,
 *                  held by the DAP debug UART, PA9 TX
 *
 * Note this MDL never mentions a pin number. It says "USART1"; the host is
 * what knows that means PA9/PA10.
 */
#include "host_api.h"

MDL_MODULE_ABI_DECLARE();
MDL_MODULE_RESOURCES(MDL_RES_UART(1));

int module_init(const host_api_t *host)
{
    host->log("if you see this, the hard-hold check did not run");
    return 0;
}
