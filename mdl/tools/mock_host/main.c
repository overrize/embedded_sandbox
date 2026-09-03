/*
 * mock_host's entry point. module.c is compiled straight into this
 * executable (see run.py) -- module_init() is an ordinary extern
 * symbol here, not something dlopen()'d.
 */
#include <stdio.h>
#include "mock_api.h"

extern int module_init(const host_api_t *host);

int main(void)
{
    const host_api_t *api = mock_api_get();

    printf("[mock_host] calling module_init()...\n");
    int ret = module_init(api);
    printf("[mock_host] module_init() returned %d\n", ret);

    mock_api_print_summary();

    /* Reaching here at all (module_init() didn't crash/abort) is the
     * pass condition -- module_init()'s own return value is module-
     * specific (the "hello" test module returns its internal counter,
     * for example), not a pass/fail signal this harness should
     * interpret on the module's behalf. */
    return 0;
}
