#ifndef MDL_PERSIST_H
#define MDL_PERSIST_H

#include <stdint.h>
#include <stdbool.h>

/*
 * F2: keep an MDL across power loss.
 *
 * The CONTRACT lives in core because core calls it (supervisor.c restores
 * at boot, registry.c carries the weak no-op defaults). The IMPLEMENTATION
 * is board-specific -- mdl/tests/hil/common/board_persist.c knows where
 * this board's flash store is and how to erase it.
 *
 * The stored thing is the original .mdl IMAGE, not the relocated arena --
 * see board_persist.c for why. Reloading it at boot therefore runs the
 * ordinary loader, so there is one code path and not a second one that
 * only ever executes at startup and only ever breaks in the field.
 */

/*
 * A SET, not one image [S5].
 *
 * With four slots, "the saved MDL" stopped being a thing: a board that
 * comes back running one of the three modules it had is not a board that
 * survived a power cut, it is a board that lost two thirds of itself.
 *
 * Entries are keyed by the image's own name, so saving a module twice
 * replaces it rather than accumulating copies -- the same rule the
 * supervisor uses to choose a slot, for the same reason.
 */

/* How many images are stored. Zero if the store is empty or failed its
 * own checks. */
uint32_t board_persist_count(void);

/* Image i, or NULL past the end. Points straight into flash; valid until
 * the next save or forget. */
const void *board_persist_entry(uint32_t i, uint32_t *out_len);

/* Add or replace one image, keyed by the name in its own header. Returns
 * false if the store is full or the write failed -- and on failure the
 * PREVIOUS set is still intact, which is the property the two-bank layout
 * exists to provide. */
bool board_persist_save(const void *image, uint32_t len);

/* Erase everything. */
bool board_persist_forget(void);

#endif /* MDL_PERSIST_H */
