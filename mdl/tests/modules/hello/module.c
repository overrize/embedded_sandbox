/*
 * Minimal test module for M1 packer/loader/GOT-relocation validation.
 * Exercises: a mutable global (.data, non-zero initializer -> forces a
 * real GOT entry + R_ARM_RELATIVE reloc), a .bss global (zero-initialized,
 * still needs a GOT slot since its address is taken), and a call into
 * the host vtable. No host resolution beyond what module_init() is
 * handed -- see mdl/host/host_api.h.
 */
#include "host_api.h"

MDL_MODULE_ABI_DECLARE();

/* Non-zero initializer forces this into real .data (not .bss), so its
 * link-time address is a genuine GOT-relocated case, not a degenerate one. */
static int g_counter = 1;

/* Zero-initialized -- lives in .bss, still needs its own GOT slot since
 * &g_last_level is taken below. */
static int g_last_level;

int module_init(const host_api_t *host)
{
    host->log("hello module: module_init running");

    g_counter += 41;
    g_last_level = host->gpio_get(0);

    host->log(g_counter == 42 ? "hello module: counter ok" : "hello module: counter WRONG");

    return g_counter;
}
