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
} mdl_fault_info_t;

extern mdl_fault_info_t g_mdl_last_fault;

void mdl_record_fault(uint32_t pc, uint32_t lr, uint32_t mmfar, uint32_t cfsr);

#endif /* MDL_REGISTRY_H */
