#ifndef MDL_LOADER_H
#define MDL_LOADER_H

#include <stddef.h>
#include "registry.h"
#include "mdl_format.h"

struct host_api; /* opaque here -- core/ doesn't need host_api.h's contents,
                   * only a pointer to hand through to the module */

typedef enum {
    MDL_LOAD_OK = 0,
    MDL_LOAD_ERR_TOO_SMALL,      /* image shorter than sizeof(mdl_header_t) */
    MDL_LOAD_ERR_BAD_MAGIC,
    MDL_LOAD_ERR_BAD_CRC,
    MDL_LOAD_ERR_ABI_MISMATCH,
    MDL_LOAD_ERR_ARCH_MISMATCH,
    MDL_LOAD_ERR_TEXT_TOO_BIG,   /* text_size exceeds the arena's text region */
    MDL_LOAD_ERR_DATA_TOO_BIG,   /* got+data+bss exceeds the arena's data region */
    MDL_LOAD_ERR_BAD_RELOC,      /* a reloc entry's got_offset falls outside the GOT area */
    MDL_LOAD_ERR_BAD_RES,        /* a resource claim is malformed or out of range */
    MDL_LOAD_ERR_RES_CONFLICT,   /* a declared resource is already owned -- see mdl_load_detail() */
} mdl_load_status_t;

/* Human-readable string for a status, for logging -- never NULL. */
const char *mdl_load_status_str(mdl_load_status_t status);

/*
 * Extra detail about the most recent mdl_load() failure, or "" when
 * there is none. Exists for MDL_LOAD_ERR_RES_CONFLICT, where the useful
 * message names the specific pin and its current owner -- something a
 * status enum cannot carry. Valid until the next mdl_load() call.
 */
const char *mdl_load_detail(void);

/*
 * Validates and copies `image` (packer.py's flat .mdl output, image_len
 * bytes) into the module arena described by *m (bounds already set by
 * sandbox_init()), applies GOT relocation, and leaves *m ready to run
 * (state = MDL_SLOT_LOADED, entry populated) -- but does NOT call
 * module_init(). Call mdl_run() separately once loading succeeds.
 *
 * *m's bounds fields (text_lo/text_hi/data_lo/data_hi/...) must already
 * be populated (sandbox_init() does this); this function only touches
 * m->state and m->entry.
 *
 * expected_abi_ver/expected_arch are supplied by the caller (not baked
 * into loader.c) so this file stays free of both host/-layer knowledge
 * (HOST_API_ABI_VERSION lives in host/host_api.h) and arch-specific
 * knowledge (which mdl_arch_t value "this build" is) -- core/ owns
 * neither. The platform's main.c is expected to pass
 * HOST_API_ABI_VERSION and MDL_ARCH_ARMV7M (or whatever this build's
 * arch is) through explicitly.
 */
mdl_load_status_t mdl_load(module_t *m, const void *image, size_t image_len,
                            uint16_t expected_abi_ver, mdl_arch_t expected_arch);

/*
 * Calls the loaded module's module_init(host), returning its result.
 * m->state must be MDL_SLOT_LOADED (i.e. mdl_load() returned MDL_LOAD_OK
 * and this hasn't already been called). Leaves m->state as
 * MDL_SLOT_RUNNING on return -- M1 runs this synchronously, privileged,
 * from the loader's own call stack (see arch_call_privileged()); M2
 * replaces the underlying call mechanism (a real unprivileged task via
 * arch_enter_unprivileged()) without changing this function's contract.
 */
int mdl_run(module_t *m, const struct host_api *host);

#endif /* MDL_LOADER_H */
