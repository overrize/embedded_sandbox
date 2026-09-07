/*
 * R3 negative case 2: one MDL, two claims, same copper.
 *
 * This is the scenario the pin-space comparison exists for. The two claims
 * share no name, no number and no kind:
 *
 *   MDL_RES_GPIO(2)   whitelist pin 2  ->  PA3   (the SW3 button)
 *   MDL_RES_UART(2)   USART2           ->  PA2 + PA3
 *
 * Comparing what they are CALLED finds nothing. Comparing the pins they
 * occupy finds it immediately, and the message says which two:
 *
 *   -> RESP_ERROR  resource conflict: USART2 (PA2 TX, PA3 RX) and
 *                  BTN0 (PA3, SW3, in, 1=idle) are both PA3
 *
 * Neither pin is held by the host -- PA3 is free for MDLs. The conflict is
 * entirely between this MDL's own two declarations, which also means it is
 * decidable from the image alone and should eventually be refused by the
 * packer rather than at load (maintain.md B2).
 */
#include "host_api.h"

MDL_MODULE_ABI_DECLARE();
MDL_MODULE_RESOURCES(MDL_RES_GPIO(2),   /* PA3 */
                     MDL_RES_UART(2));  /* PA2 + PA3 */

int module_init(const host_api_t *host)
{
    host->log("if you see this, the self-conflict check did not run");
    return 0;
}
