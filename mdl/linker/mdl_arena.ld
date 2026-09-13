/*
 * mdl/linker/mdl_arena.ld
 *
 * Module sandbox arena. INCLUDE this file from the main AT32F435 linker
 * script, inside the top-level SECTIONS block, after the host's own
 * .text/.data/.bss output sections. "RAM" below must name the same
 * memory region your host script already uses for .data/.bss -- rename
 * the ">RAM" at the bottom if your script calls it something else.
 *
 * Layout (v1, fixed, no dynamic sub-allocation -- see mdl/core/sandbox.c,
 * which is the only file that should need to change if this ever grows
 * dynamic sub-allocation):
 *
 *   offset    size    content                          permission
 *   0x0000    16K     module text                      RO + exec, unpriv
 *   0x4000     8K      module data/bss/GOT              RW + XN,   unpriv
 *   0x6000     8K      module heap+stack                RW + XN,   unpriv
 *   0x8000    32B     guard (stack-overflow backstop)   no access
 *   0x8020    ~32K    reserved / unused in v1
 *   (arena total: 64K, per the ALIGN below)
 *
 * ARMv7-M MPU: a region's base address must be aligned to its own size,
 * and its size must be a power of two. The offsets above are exact
 * powers of two chosen so each sub-region is self-aligned given the
 * arena's own 64K alignment -- do not reorder or resize any of them
 * without re-checking that alignment still holds for every region that
 * follows it.
 */

/*
 * S1: the arena is now ONE naturally-aligned pool that mdl/core/buddy.c
 * hands out, rather than four fixed slabs for one module.
 *
 * The fixed layout could only ever hold one MDL, and it wasted half its 64K
 * on alignment padding it could not use. A 128K pool aligned to 128K keeps
 * the property the MPU needs -- every block the allocator returns inherits
 * natural alignment from the pool's base -- while letting each module take
 * only the orders its own text and data actually require.
 *
 * The __mdl_*_start/end symbols stay for now: the loader still carves a
 * single module out of the pool's front, and moving it onto the allocator
 * is the rest of S1. Defining them in terms of the pool keeps one source of
 * truth while that transition happens, instead of two layouts that must be
 * kept in agreement.
 */
.mdl_arena (NOLOAD) : ALIGN(131072)
{
    __arena_start = .;
    __mdl_pool_start = .;

    __mdl_text_start = .;
    . += 16K;
    __mdl_text_end = .;

    __mdl_data_start = .;
    . += 8K;
    __mdl_data_end = .;

    __mdl_heap_stack_start = .;
    . += 8K;
    __mdl_heap_stack_end = .;

    __mdl_guard_start = .;
    . += 32;
    __mdl_guard_end = .;

    . = __arena_start + 128K;
    __mdl_pool_end = .;
    __arena_end = .;
} > RAM
