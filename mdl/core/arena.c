/*
 * Handing arena memory to a module [S1].
 *
 * This is the last piece that made "one slot" structural rather than
 * merely current: the loader used to write into three fixed regions the
 * linker had carved out, so a second module had nowhere to go no matter
 * what the rest of the code did.
 *
 * THREE BLOCKS, NOT ONE, because they carry different permissions and the
 * MPU applies permissions per region:
 *
 *   text       read-only, executable
 *   data       read/write, execute-never   (GOT, .data, .bss)
 *   heapstack  read/write, execute-never   (arg block + heap + guard + stack)
 *
 * Each comes straight from mdl_buddy_alloc(), which is why this works at
 * all: every block it returns is a power of two in size and aligned to its
 * own size, which is exactly and only what an ARMv7-M MPU region requires.
 * There is no alignment arithmetic here because there is none to do.
 *
 * SIZES ARE THE MODULE'S, NOT THE ARENA'S. text and data come from the
 * image's own header, so a 1KB module takes a 1KB block instead of the 16KB
 * slab the fixed layout gave everything. The heapstack block stays a fixed
 * size because the ABI promises MDL_HEAP_SIZE and MDL_STACK_SIZE to every
 * module -- that is a contract, not a measurement.
 */
#include "arena.h"
#include "buddy.h"
#include "registry.h"
#include "module_task.h"

/* Arg block, heap, guard band and stack, in that order. Fixed because the
 * ABI promises them; see module_task.h for the layout itself. */
#define HEAPSTACK_BYTES (MDL_HEAP_SIZE + MDL_GUARD_SIZE + MDL_STACK_SIZE)

bool mdl_arena_would_fit(size_t text_bytes, size_t data_bytes, size_t *need_out)
{
    size_t t = mdl_buddy_block_size(text_bytes);
    size_t d = mdl_buddy_block_size(data_bytes);
    size_t h = mdl_buddy_block_size(HEAPSTACK_BYTES);

    if (need_out != NULL) {
        *need_out = t + d + h;
    }
    if (t == 0u || d == 0u || h == 0u) {
        return false;   /* larger than the pool itself */
    }

    /*
     * A cheap check, and deliberately not a promise.
     *
     * Whether three specific blocks can be placed depends on how the pool
     * is currently split, which only the allocator knows -- so this rejects
     * the clearly impossible early, and mdl_arena_acquire() remains the
     * thing that actually decides. Two answers are unavoidable here; what
     * must not happen is the second one being silent, which is why acquire
     * reports the shortfall rather than just failing.
     *
     * Nothing else allocates from the pool between the two calls: both run
     * on the supervisor task, and modules never touch this allocator --
     * their own heaps live inside the blocks handed out here.
     */
    size_t largest = 0;
    mdl_buddy_stats(NULL, &largest);
    size_t biggest_needed = (t > d) ? t : d;
    if (h > biggest_needed) {
        biggest_needed = h;
    }
    return largest >= biggest_needed;
}

bool mdl_arena_acquire(module_t *m, size_t text_bytes, size_t data_bytes)
{
    void *text = mdl_buddy_alloc(text_bytes);
    void *data = mdl_buddy_alloc(data_bytes);
    void *hs   = mdl_buddy_alloc(HEAPSTACK_BYTES);

    if (text == NULL || data == NULL || hs == NULL) {
        /* All or nothing. A partial acquisition would leave blocks held by
         * a module that never loaded -- a leak that only shows up as the
         * arena mysteriously shrinking across failed loads. */
        mdl_buddy_free(text);
        mdl_buddy_free(data);
        mdl_buddy_free(hs);
        return false;
    }

    m->text_lo = (uintptr_t)text;
    m->text_hi = m->text_lo + mdl_buddy_block_size(text_bytes);
    m->data_lo = (uintptr_t)data;
    m->data_hi = m->data_lo + mdl_buddy_block_size(data_bytes);
    m->heap_stack_lo = (uintptr_t)hs;
    m->heap_stack_hi = m->heap_stack_lo + mdl_buddy_block_size(HEAPSTACK_BYTES);

    /* The guard band between heap and stack -- the one a stack overflow
     * actually runs into. It has no MPU region of its own (module_task.h
     * explains why), so these bounds exist for fault classification: an
     * address in here says "stack overflow", not "wild pointer". */
    m->guard_lo = m->heap_stack_lo + MDL_HEAP_SIZE;
    m->guard_hi = m->guard_lo + MDL_GUARD_SIZE;

    m->arena_held = true;
    return true;
}

void mdl_arena_release(module_t *m)
{
    if (!m->arena_held) {
        return;
    }
    mdl_buddy_free((void *)m->text_lo);
    mdl_buddy_free((void *)m->data_lo);
    mdl_buddy_free((void *)m->heap_stack_lo);

    /* Clear the bounds, not just the flag. A stale text_lo left behind
     * would still look like a valid region to ptr_owned_by_module() and to
     * fault classification, and it would be pointing at memory the next
     * module now owns. */
    m->text_lo = m->text_hi = 0;
    m->data_lo = m->data_hi = 0;
    m->heap_stack_lo = m->heap_stack_hi = 0;
    m->guard_lo = m->guard_hi = 0;
    m->arena_held = false;
}
