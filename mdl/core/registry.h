#ifndef MDL_REGISTRY_H
#define MDL_REGISTRY_H

#include <stdint.h>
#include <stdbool.h>
#include "mdl_format.h" /* MDL_CMD_NAME_MAX, mdl_res_t */

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

    /*
     * When the MDL last gave evidence of being alive [ABI v3 semantics].
     *
     * It used to be bumped by ANY host call, which sounds reasonable and
     * is not: a module stuck in `while (1) { host->uptime_ms(); }` feeds
     * it forever. 'Called an API' is evidence of executing, not of making
     * progress. Only two things count now:
     *
     *   host->watchdog_feed()  -- said so explicitly
     *   returning from delay_ms() -- was blocked in the host, which by
     *                                definition is not hung
     *
     * The cost is that an MDL doing more than MDL_WATCHDOG_TIMEOUT_MS of
     * uninterrupted work must say so. That is the contract, and it is the
     * only version of this that detects anything.
     */
    uint32_t last_active_tick;

    /*
     * Tick until which the MDL is legitimately blocked inside the host
     * (delay_ms today; a blocking event wait once F1 lands), or 0.
     *
     * Without this the watchdog kills anything that sleeps longer than
     * the timeout -- and sleeping is not hanging. This is the field that
     * makes a resident, event-driven MDL possible at all.
     */
    uint32_t parked_until_tick;

    /* ---- ABI v2: what the module declared about itself ---- */

    /* module_cmd(), or NULL when the module exports no console command.
     * Thumb bit already set, same as `entry`. */
    void *cmd_entry;
    char  cmd_name[MDL_CMD_NAME_MAX];

    /* What `status` calls the thing currently loaded. */
    char  name[MDL_NAME_MAX];

    /* Bit i set = the module declared whitelist GPIO pin i. Checked at
     * load time against what the host already owns, and again on every
     * gpio_set/gpio_get call -- a module that declares one pin and
     * drives another is refused at the call, not merely at the load. */
    uint32_t gpio_claimed;

    /* Return value of the most recent module_cmd() invocation. Written
     * by the module task through the syscall gate, read by the
     * supervisor once the slot goes back to MDL_SLOT_LOADED. */
    int cmd_ret;
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
