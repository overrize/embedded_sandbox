#ifndef MDL_REGISTRY_H
#define MDL_REGISTRY_H

#include <stdint.h>
#include <stdbool.h>

/*
 * Module slot bookkeeping. v1 has exactly one slot -- the arena (see
 * mdl/linker/mdl_arena.ld) only has room laid out for one module at a
 * time. No CMSIS or arch types appear here; the arch layer talks to this
 * file only through mdl_record_fault()'s plain-integer arguments.
 */

typedef enum {
    MDL_SLOT_EMPTY = 0,
    MDL_SLOT_LOADED,
    MDL_SLOT_RUNNING,
    MDL_SLOT_FAULTED,
} mdl_slot_state_t;

struct module {
    mdl_slot_state_t state;
    uintptr_t text_lo, text_hi;
    uintptr_t data_lo, data_hi;
    uintptr_t heap_stack_lo, heap_stack_hi;
    uintptr_t guard_lo, guard_hi;
    void *entry; /* set at load time (M1); NULL while the slot is empty */

    /* M3: fault recovery + software watchdog bookkeeping.
     *
     * task_handle is FreeRTOS's TaskHandle_t, kept as a bare void* here
     * rather than pulling FreeRTOS.h/task.h into this header -- it's an
     * opaque handle to core/ either way, and this file's own stated
     * goal ("no CMSIS or arch types appear here") extends naturally to
     * "no RTOS types either", even though other core/ files (loader.c,
     * module_task.c) do call the RTOS API directly (the RTOS is fixed
     * project infrastructure, not something arch-abstracted -- see
     * module_task.h's comment). Cast to TaskHandle_t at the point of
     * use (mdl/core/module_task.c sets it, mdl/core/supervisor.c reads
     * it back for vTaskDelete()).
     */
    void *task_handle;

    /* Bumped by host_api.c on every successful SVC-gated call the
     * module makes (mdl/host/host_api.c's mdl_watchdog_feed()) --
     * "the module made a host call recently" doubles as "the module
     * isn't stuck in a tight loop with no host interaction", which is
     * exactly the while(1){} case the spec's software watchdog test
     * needs to catch. A module doing long silent CPU-bound work with no
     * host calls looks the same as hung by this metric -- a real v1
     * limitation, not an oversight; the vtable the spec hands modules
     * has no explicit "feed" call, so implicit-feed-via-any-host-call
     * is the only signal available without inventing a new ABI entry. */
    uint32_t last_active_tick;
};
typedef struct module module_t;

/* v1: exactly one module slot. */
extern module_t g_mdl_slot;

void registry_init(void);

/*
 * Fault record, filled in by the arch layer's exception handler and read
 * back by the loader task (M3) or, for now, by the M0 self-test. Plain
 * integers only, deliberately -- see the file comment above.
 */
typedef struct {
    bool     occurred;
    uint32_t pc;
    uint32_t lr;
    uint32_t mmfar;
    uint32_t cfsr;
    uint32_t text_offset; /* pc - module text start, so the PC side can
                            * addr2line straight into the module's own
                            * ELF -- 0xFFFFFFFF if pc wasn't inside any
                            * loaded module's text (a host-side fault). */
} mdl_fault_info_t;

extern mdl_fault_info_t g_mdl_last_fault;

/* Records fault details and classifies pc against the loaded module's
 * text bounds. Returns true if pc fell inside the module (a module
 * fault, recoverable by killing just that task) or false otherwise (a
 * host-side bug -- unrecoverable, the caller should reset). */
bool mdl_record_fault(uint32_t pc, uint32_t lr, uint32_t mmfar, uint32_t cfsr);

#endif /* MDL_REGISTRY_H */
