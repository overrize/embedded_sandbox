#ifndef MDL_BUDDY_H
#define MDL_BUDDY_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/*
 * Buddy allocator for the module arena [S1].
 *
 * WHY BUDDY AND NOT SOMETHING SIMPLER. The ARMv7-M MPU requires every
 * region to be a power of two in size AND naturally aligned to its own
 * size. That is normally a burden; here it is exactly the precondition a
 * buddy allocator already works under, so the constraint costs nothing and
 * buys back the property that matters most: blocks COALESCE WHEN FREED.
 * Unloading a module hands its space back as a usable large block rather
 * than as rubble, which is what makes load/unload cycles survivable.
 *
 * WHY NOT COMPACT. Defragmenting would mean relocating a loaded module,
 * and a running module's stack and heap hold pointers into itself that
 * nothing outside can rewrite. Tracking those is what a garbage collector
 * does, and a tracker either misses pointers (corruption) or over-retains
 * them (a leak) -- this project declined a GC for the same reasons it
 * declines an interpreter.
 *
 * That risk does not arise here, because the granularity is one block per
 * module: allocated whole, freed whole, with the module's own heap and
 * stack inside it. Nothing outside a block points into it except a handful
 * of host-side slot fields that reclaim_module() already clears.
 *
 * So internal fragmentation is the honest cost -- a 5K module occupies an
 * 8K block -- and it is REPORTED rather than hidden: mdl_buddy_stats()
 * gives both the total free and the largest single block, so a refusal can
 * say "12K free, largest 8K, this needs 16K" instead of failing opaquely.
 */

/* 1K. Below this the bookkeeping costs more than it saves, and no useful
 * module region is smaller. */
#define MDL_BUDDY_MIN_ORDER 10

/* 128K of arena. Sized against ~245K of SRAM left after the firmware, with
 * room to spare -- deliberately not "all of it", since running out of heap
 * for the host is a far worse failure than refusing to load a module. */
#define MDL_BUDDY_MAX_ORDER 17

#define MDL_BUDDY_MIN_SIZE ((size_t)1u << MDL_BUDDY_MIN_ORDER)
#define MDL_BUDDY_POOL_SIZE ((size_t)1u << MDL_BUDDY_MAX_ORDER)

/* Number of minimum-sized slots the pool divides into; the side table has
 * one entry each. */
#define MDL_BUDDY_SLOTS (1u << (MDL_BUDDY_MAX_ORDER - MDL_BUDDY_MIN_ORDER))

/*
 * Hand the allocator its pool. base must itself be MDL_BUDDY_POOL_SIZE
 * aligned -- the whole point is that every block it returns is naturally
 * aligned, and that property is inherited from the pool's own base.
 */
void mdl_buddy_init(uintptr_t base);

/*
 * Smallest naturally-aligned block of at least `size` bytes, or NULL.
 *
 * The returned pointer IS the block's base and IS aligned to the block's
 * (rounded-up, power-of-two) size, which is what lets it be handed
 * straight to an MPU region without further adjustment. There is no header
 * in front of it -- a header would push the usable area off alignment,
 * which is why the size bookkeeping lives in a side table instead.
 */
void *mdl_buddy_alloc(size_t size);

/*
 * Give a block back. p must be exactly what alloc returned; a pointer into
 * the middle of a block, or one already freed, is ignored rather than
 * acted on -- corrupting the allocator is a far worse outcome than leaking
 * one block, and a double free is a bug in the caller that should not
 * become memory corruption here.
 */
void mdl_buddy_free(void *p);

/* The actual size reserved for a block, which is the request rounded up to
 * a power of two and to MDL_BUDDY_MIN_SIZE. Exposed so a caller can report
 * the internal fragmentation it is about to pay before paying it. */
size_t mdl_buddy_block_size(size_t size);

/*
 * free_total   bytes not currently allocated, summed across all orders
 * largest      the biggest single block that could still be allocated
 *
 * Both matter, and separately: free_total says whether the arena is full,
 * largest says whether it is fragmented. A refusal that reports only the
 * first is the mysterious kind.
 */
void mdl_buddy_stats(size_t *free_total, size_t *largest);

/* Self-check, for the console command. Returns the number of failed
 * assertions; zero means the allocator behaved. Runs on a scratch pool so
 * it cannot disturb a loaded module. */
int mdl_buddy_selftest(char *detail, size_t detail_len);

#endif /* MDL_BUDDY_H */
