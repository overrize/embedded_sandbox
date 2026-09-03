/*
 * Negative test 6/6: try to hijack execution directly into HOST code --
 * specifically, into .privileged_functions (the real FreeRTOS kernel
 * implementation + this project's own privileged-only code), bypassing
 * the SVC gate entirely. This is the attack the freertos_system_calls /
 * privileged_functions split exists to stop: even with a raw function
 * pointer to privileged code, an unprivileged task can't fetch
 * instructions from there -- portPRIVILEGED_FLASH_REGION (region 1) has
 * no unprivileged-execute permission. Expect: MemManage fault (an
 * execute-permission violation, not a data access one -- same handler,
 * same recovery path).
 *
 * PRIVILEGED_ADDR below is a real measured address (see
 * mdl/tests/hil/at32f435_m3/build/firmware.map after linking against
 * the "hello" module, or `nm firmware.elf | grep __privileged_functions_start__`)
 * with the thumb bit set (+1) so a `blx` to it doesn't itself fault on a
 * bad interworking check before ever reaching the MPU test this is
 * actually trying to exercise. If you change what's compiled into
 * .privileged_functions, this address may need updating -- it is NOT
 * derived from a linker symbol the module itself can see (modules don't
 * link against the host's linker script at all), so it has to be a
 * literal here, by design.
 */
#include "host_api.h"

MDL_MODULE_ABI_DECLARE();

#define PRIVILEGED_ADDR 0x08008001u /* verify against your own build's map */

int module_init(const host_api_t *host)
{
    host->log("fault_jump_to_host: about to call into .privileged_functions");
    void (*hijack)(void) = (void (*)(void))PRIVILEGED_ADDR;
    hijack(); /* should never return */
    host->log("fault_jump_to_host: SHOULD NOT REACH HERE");
    return -1;
}
