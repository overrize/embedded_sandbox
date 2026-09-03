#ifndef MDL_HOST_API_H
#define MDL_HOST_API_H

#include <stdint.h>
#include <stddef.h>

/*
 * host_api.h -- THE capability boundary for modules.
 *
 * This header is the only thing a module author (human or agent) needs
 * to read to know what a module can do. A module never resolves host
 * symbols by name and never links against host code directly -- the only
 * way in is the host_api_t pointer handed to module_init(). If a
 * capability isn't listed here, the module cannot do it, full stop --
 * there is no escape hatch, no "just call the driver directly."
 *
 * ABI STABILITY: host_api_t is a versioned, append-only struct. abi_ver
 * is checked by the packer at pack time and by the loader at load time;
 * a module built against a different abi_ver than the running host is
 * rejected before it ever executes. Existing fields never change type,
 * meaning, or position -- new capabilities are added at the end, behind
 * a version bump.
 *
 * CALLING CONVENTION (from M2 onward): every function here is an SVC
 * trampoline. Calling it costs a full privilege-escalation round trip
 * (svc -> handler -> validate args -> do the thing -> return), not a
 * plain function call. Do not call these in a tight loop expecting
 * plain-call latency. (M1: modules run privileged and these are plain
 * function calls -- still correct to code against this contract, since
 * M2 changes only the plumbing, not the semantics or signatures below.)
 *
 * THREAD CONTEXT: every function below is documented for whether it is
 * safe to call from module_init() (loader task context, still starting
 * up), from the module's own task body (normal running context), and
 * from a host-invoked module callback (e.g. a registered timer/IRQ
 * bottom-half -- module callbacks always run in the module's own task
 * context via a host-side dispatch, never in interrupt context; there is
 * currently no callback-registration API in this header, so this
 * distinction is forward-looking).
 */

#define HOST_API_ABI_VERSION 1

/*
 * Every module source file must invoke this exactly once at file scope.
 * It exports a small const symbol (__mdl_abi_ver) that tools/packer.py
 * reads directly out of the compiled .so and compares against ITS OWN
 * notion of HOST_API_ABI_VERSION (parsed from this same header) --
 * catching "module was compiled against a stale copy of host_api.h" as
 * a pack-time error instead of a load-time mystery:
 *   abi_ver mismatch: module built against v2, host expects v3
 * Without this, a version drift between the header a module happened to
 * be compiled with and the header the packer/firmware were built with
 * would go undetected until (or past) runtime.
 */
#define MDL_MODULE_ABI_DECLARE() \
    __attribute__((used)) const uint32_t __mdl_abi_ver = HOST_API_ABI_VERSION

typedef struct host_api {
    uint32_t abi_ver; /* Always HOST_API_ABI_VERSION for the struct
                        * layout you are compiling against. Set by the
                        * host before module_init() is called; a module
                        * should assert this matches the value it was
                        * built with (packer.py already enforces this at
                        * pack time, so a mismatch here would mean the
                        * running firmware's host_api.h and the module's
                        * are out of sync -- treat it as fatal). */

    /*
     * log(msg)
     *   Writes a diagnostic line to the host's log sink (in v1: USB CDC
     *   / semihosting, whichever the firmware build enables).
     *   - msg: NUL-terminated string. Must point inside the module's own
     *     data/heap/stack region (checked by the SVC handler in M2 --
     *     see host_api.h's validation note below); a string literal in
     *     the module's own .rodata (part of its text region) qualifies.
     *   - Length: truncated at an implementation-defined limit (v1: 127
     *     bytes) rather than rejected -- a too-long message still logs,
     *     just cut short.
     *   - Callable from module_init() and from the module's task body.
     *   - Never blocks longer than it takes to enqueue the line; does
     *     not wait for the line to actually be transmitted over USB.
     */
    void (*log)(const char *msg);

    /*
     * gpio_set(pin, level)
     *   - pin: must be one of the pins in this firmware build's
     *     compile-time GPIO whitelist (see host/host_api.c's
     *     g_gpio_whitelist). Any other value returns -1 and touches no
     *     hardware -- it does NOT fault the module, so check the return
     *     value rather than relying on a fault to catch a bad pin.
     *   - level: 0 or 1. Any other value is treated as "nonzero -> 1".
     *   - Returns 0 on success, -1 if pin is not whitelisted.
     *   - Callable from module_init() and from the module's task body.
     *   - A pin is never implicitly shared: only one loaded module may
     *     claim a given pin's whitelist slot at a time in a future
     *     multi-module build; v1 has one module slot, so this doesn't
     *     yet bite, but don't assume future versions let two modules
     *     drive the same pin.
     */
    int (*gpio_set)(int pin, int level);

    /*
     * gpio_get(pin)
     *   - pin: same whitelist rule as gpio_set.
     *   - Returns 0 or 1 (the pin's input level) on success, -1 if pin
     *     is not whitelisted. -1 is not a valid level, so it's
     *     unambiguous as an error signal here.
     *   - Callable from module_init() and from the module's task body.
     */
    int (*gpio_get)(int pin);

    /*
     * delay_ms(ms)
     *   - Blocks the calling module task for at least ms milliseconds.
     *     This is a cooperative delay (it yields the CPU to other tasks
     *     while waiting), not a busy-loop -- it costs the module
     *     nothing but wall-clock time and priority-band scheduling
     *     fairness.
     *   - ms: 0 is legal (yields once, returns as soon as rescheduled).
     *     No enforced upper bound in v1, but see the software watchdog
     *     note: a module that never returns from a host call for longer
     *     than the watchdog period gets killed exactly as if it were
     *     spinning in its own code, so passing an enormous ms is
     *     equivalent to hanging.
     *   - Callable from module_init() (from M2 on, module_init() itself
     *     runs inside the module's own FreeRTOS task -- see
     *     mdl/core/module_task.c -- so there is always a task context to
     *     yield from) and from the module's task body.
     */
    void (*delay_ms)(uint32_t ms);

    /*
     * alloc(n) / free(p)
     *   - Allocates from this module's own private pool, carved out of
     *     its heap+stack arena region -- never the host's own heap.
     *     alloc() returns NULL if the pool is exhausted or n is larger
     *     than the pool's total capacity; it does not fault, and it
     *     does not touch any other module's or the host's memory.
     *   - free(p): p must be a pointer previously returned by this same
     *     module's alloc() and not already freed. Freeing a foreign or
     *     already-freed pointer is rejected by the host's bookkeeping
     *     (parameter validation, M2) rather than corrupting the pool.
     *   - The entire pool is force-reclaimed when the module is
     *     unloaded, whether or not the module called free() on
     *     everything -- a module leaking its own allocations only wastes
     *     its own pool budget until unload, it can't leak host memory.
     *   - Callable from module_init() and from the module's task body.
     */
    void *(*alloc)(size_t n);
    void  (*free)(void *p);

    /*
     * uptime_ms()
     *   - Milliseconds since host boot. Monotonic, wraps at UINT32_MAX
     *     (~49.7 days) -- do not assume it never wraps; compute
     *     durations with subtraction (wraparound-safe for uint32_t),
     *     never with a direct greater-than comparison against a stored
     *     absolute deadline.
     *   - Callable from module_init() and from the module's task body.
     */
    uint32_t (*uptime_ms)(void);
} host_api_t;

/*
 * Host-side (not module-facing) support functions -- these are NOT part
 * of the module ABI surface and a module never sees their declarations
 * or calls them. main.c/loader-integration code uses these directly.
 */
struct module; /* registry.h's module_t, kept opaque here */

/* The one host_api_t instance handed to every loaded module in v1
 * (single module slot -> one vtable instance is enough). Populated once
 * at boot by host_api_init(). */
extern const host_api_t g_host_api;

void host_api_init(void);

/* Resets this module's private alloc() pool to empty, carved out of
 * m's own heap+stack arena region (see host_api.c's HEAP_FRACTION
 * comment for the heap/stack split). Call once, after a successful
 * mdl_load() and before mdl_run() -- loader.c itself doesn't call this,
 * since core/ doesn't know host_api.c exists; the platform's main.c
 * sequences load -> host_api_pool_reset -> run explicitly. */
void host_api_pool_reset(struct module *m);

#endif /* MDL_HOST_API_H */
