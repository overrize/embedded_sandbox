#ifndef MDL_MOCK_API_H
#define MDL_MOCK_API_H

#include "host_api.h"

/*
 * Builds a host_api_t backed by real, native implementations -- printf
 * for log(), real malloc()/free() for alloc()/free() (so a leak/double-
 * free/use-after-free is whatever ASan or the platform's own heap
 * debugger would catch, not something this mock has to reimplement), a
 * small in-process pin-state array for gpio_set()/gpio_get(), and the
 * host's real clock for uptime_ms().
 *
 * Deliberately NOT a dynamically-loaded .so/.dll: module.c is compiled
 * and linked straight into the mock_host executable (see run.py) --
 * simpler than reproducing dlopen/LoadLibrary on every platform, and it
 * loses nothing meaningful, since the one thing a host-native build
 * can never exercise anyway is the ARM -fPIC/-msingle-pic-base
 * codegen + GOT relocation (that's what tools/packer.py's own mock
 * verification is for, plus real hardware) -- mock_host's job is
 * catching MODULE LOGIC bugs at full native speed with a real
 * debugger/backtrace, which this approach does just as well.
 */
const host_api_t *mock_api_get(void);

/* Prints a one-line summary of everything the mock observed (final
 * gpio pin states, outstanding un-freed allocations, log line count) --
 * called by main.c after module_init() returns, so a human/agent
 * reading mock_host's output doesn't have to infer module behavior
 * purely from log() lines. */
void mock_api_print_summary(void);

#endif /* MDL_MOCK_API_H */
