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

/* The saved image, or NULL if the store is empty or fails its own CRC.
 * Points straight into flash; valid until the next save/forget. */
const void *board_persist_image(uint32_t *out_len);

/* Erase, write image, write header LAST -- so a power cut during a save
 * leaves the record absent rather than valid-but-half-written. */
bool board_persist_save(const void *image, uint32_t len);

bool board_persist_forget(void);
uint32_t board_persist_capacity(void);

#endif /* MDL_PERSIST_H */
