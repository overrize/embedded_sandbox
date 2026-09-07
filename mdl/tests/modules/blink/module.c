/*
 * Demonstrates the two ABI v2 features together:
 *
 *   1. It ADDS A COMMAND to the device. Before this module is pushed,
 *      `blink` is not something the console knows; after, `help` lists it
 *      and typing `blink 3` runs code that was never flashed.
 *   2. It DECLARES the hardware it touches. Pin 1 (LEDG) has no host
 *      owner, so the claim is granted. See ../conflict/module.c for the
 *      other outcome.
 *
 * Pair it with ../conflict/module.c; the two together are the whole test:
 * one loads, one is refused, and the refusal names the reason.
 */
#include "host_api.h"

MDL_MODULE_ABI_DECLARE();
MDL_MODULE_COMMAND("blink");
MDL_MODULE_RESOURCES(MDL_RES_GPIO(1));

/*
 * Lives in the module's data region, which the host does not touch
 * between invocations -- so this really does count across separate
 * `blink` commands, and is the simplest visible proof that a module is a
 * resident thing rather than a one-shot script. Each module_cmd() call
 * does get a fresh STACK, though; only globals persist.
 */
static int g_total_blinks = 3;

int module_init(const host_api_t *host)
{
    host->log("blink module resident; type `blink [n]` at the console");
    /* Deliberately non-zero so a reader can tell relocated .data from
     * zeroed .bss when dumping the arena with `mem`. */
    return g_total_blinks;
}

int module_cmd(const host_api_t *host, int argc, const char *const *argv)
{
    /* argv[0] is the command name, so this reads like main(). The strings
     * are in the module's own arg block -- the host copied them there,
     * because the console's line buffer is host memory this task cannot
     * read at all. */
    int n = (argc > 1) ? host->atoi(argv[1]) : 1;
    if (n < 1) {
        n = 1;
    }
    if (n > 20) {
        n = 20; /* the supervisor kills a command that outstays 5s */
    }

    for (int i = 0; i < n; i++) {
        host->gpio_set(1, 0); /* LEDG on -- active low */
        host->delay_ms(120);
        host->gpio_set(1, 1);
        host->delay_ms(120);
    }

    g_total_blinks += n;
    host->log("blink done");
    return g_total_blinks; /* the console prints this back */
}
