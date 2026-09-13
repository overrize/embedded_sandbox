#ifndef MDL_ARENA_H
#define MDL_ARENA_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

struct module;

/*
 * Giving a module its memory [S1]. See arena.c for why three blocks.
 *
 * The split between "would it fit" and "take it" mirrors F3's split
 * between validating an image and committing to it: nothing may be
 * destroyed or claimed until the whole image is known to be loadable.
 */

/* Could an image of this shape be placed at all? Cheap and conservative --
 * it rejects the impossible, and acquire remains the real decision.
 * need_out receives the total bytes the three blocks would occupy, which is
 * what a refusal should quote back. */
bool mdl_arena_would_fit(size_t text_bytes, size_t data_bytes, size_t *need_out);

/* Take the three blocks and fill in the module's bounds, or take none and
 * leave it untouched. There is no partial outcome. */
bool mdl_arena_acquire(struct module *m, size_t text_bytes, size_t data_bytes);

/* Give them back and clear the bounds. Safe to call on a module that holds
 * nothing. */
void mdl_arena_release(struct module *m);

#endif /* MDL_ARENA_H */
