/*
 * Fixed v1 arena layout -- mirrors mdl/linker/mdl_arena.ld exactly. No
 * dynamic sub-allocation yet (that's a post-v1 item); when it lands, this
 * should be the only file that needs to change -- arch_setup_regions()
 * and everything under arch/ stay untouched.
 *
 * Region table:
 *   text        16K   RO + exec, unprivileged
 *   data        8K    RW + no-exec, unprivileged   (data/bss/GOT)
 *   heap+stack  8K    RW + no-exec, unprivileged
 *   guard       32B   no access, any privilege
 */
#include "sandbox.h"
#include "arch_if.h"
#include "registry.h"

extern uint8_t __mdl_text_start[], __mdl_text_end[];
extern uint8_t __mdl_data_start[], __mdl_data_end[];
extern uint8_t __mdl_heap_stack_start[], __mdl_heap_stack_end[];
extern uint8_t __mdl_guard_start[], __mdl_guard_end[];

void sandbox_bounds_init(void)
{
    /* Record the bounds so fault classification (M3), the console's
     * `arena` command, and mdl_load()'s destination addresses all have
     * somewhere authoritative to read them from. The slot itself stays
     * MDL_SLOT_EMPTY -- bounds being known is not the same as a module
     * being loaded. */
    g_mdl_slot.text_lo       = (uintptr_t)__mdl_text_start;
    g_mdl_slot.text_hi       = (uintptr_t)__mdl_text_end;
    g_mdl_slot.data_lo       = (uintptr_t)__mdl_data_start;
    g_mdl_slot.data_hi       = (uintptr_t)__mdl_data_end;
    g_mdl_slot.heap_stack_lo = (uintptr_t)__mdl_heap_stack_start;
    g_mdl_slot.heap_stack_hi = (uintptr_t)__mdl_heap_stack_end;
    g_mdl_slot.guard_lo      = (uintptr_t)__mdl_guard_start;
    g_mdl_slot.guard_hi      = (uintptr_t)__mdl_guard_end;
}

void sandbox_init(void)
{
    /*
     * Highest priority first (index 0): the guard region must win any
     * accidental overlap with its neighbors, so it leads the array even
     * though arch_setup_regions() is free to map "priority" onto whatever
     * numbering scheme the hardware uses (ARM: higher region number wins;
     * see mdl_region_t's comment in mdl_format.h).
     */
    const mdl_region_t regions[] = {
        {
            .base = __mdl_guard_start,
            .size = (uint32_t)(__mdl_guard_end - __mdl_guard_start),
            .perm = MDL_PERM_NONE,
            .privileged_only = false,
        },
        {
            .base = __mdl_text_start,
            .size = (uint32_t)(__mdl_text_end - __mdl_text_start),
            .perm = MDL_PERM_READ | MDL_PERM_EXEC,
            .privileged_only = false,
        },
        {
            .base = __mdl_data_start,
            .size = (uint32_t)(__mdl_data_end - __mdl_data_start),
            .perm = MDL_PERM_READ | MDL_PERM_WRITE,
            .privileged_only = false,
        },
        {
            .base = __mdl_heap_stack_start,
            .size = (uint32_t)(__mdl_heap_stack_end - __mdl_heap_stack_start),
            .perm = MDL_PERM_READ | MDL_PERM_WRITE,
            .privileged_only = false,
        },
    };

    arch_setup_regions(regions, sizeof(regions) / sizeof(regions[0]));

    sandbox_bounds_init();
}
