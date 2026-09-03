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

.mdl_arena (NOLOAD) : ALIGN(65536)
{
    __arena_start = .;

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

    . = __arena_start + 64K;
    __arena_end = .;
} > RAM
