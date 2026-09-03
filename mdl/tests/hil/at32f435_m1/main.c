/*
 * M1 acceptance firmware: load a packed module (embedded as a linked-in
 * .rodata blob via objcopy -- USB CDC transport doesn't exist until M4,
 * so for now the module image is baked into the firmware at build time
 * instead of arriving over the wire at runtime; see the Makefile) and
 * run it, privileged, via mdl_run(). This whole embedding mechanism is
 * an M1-only test shim, not part of the permanent design -- M4 replaces
 * it with a USB CDC receive buffer, at which point this main.c's
 * load/run sequence is what a "module received" handler calls.
 *
 * NOT WIRED TO A BUILD YET beyond this project's own Makefile -- see
 * mdl/tests/hil/at32f435_m0/main.c's file comment for the same caveats
 * (no startup/CMSIS of our own, real AT32F435 BSP required). This one
 * additionally needs mdl/tests/modules/hello/build/module.mdl to exist
 * (built + packed first -- see the Makefile's mdl_blob target).
 */
#include <stdint.h>
#include "sandbox.h"
#include "registry.h"
#include "loader.h"
#include "host_api.h"

extern const uint8_t _binary_module_mdl_start[];
extern const uint8_t _binary_module_mdl_end[];

int main(void)
{
    registry_init();
    sandbox_init();
    host_api_init();

    size_t image_len = (size_t)(_binary_module_mdl_end - _binary_module_mdl_start);
    mdl_load_status_t st = mdl_load(&g_mdl_slot, _binary_module_mdl_start, image_len,
                                     HOST_API_ABI_VERSION, MDL_ARCH_ARMV7M);

    g_host_api.log(mdl_load_status_str(st));

    if (st == MDL_LOAD_OK) {
        host_api_pool_reset(&g_mdl_slot);
        int ret = mdl_run(&g_mdl_slot, &g_host_api);
        (void)ret; /* module_init's return value -- inspect under a debugger;
                    * "hello" module returns g_counter, expected to be 42 */
    }

    for (;;) {
        __asm volatile("bkpt #0");
    }
}
