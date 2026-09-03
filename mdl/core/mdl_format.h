#ifndef MDL_FORMAT_H
#define MDL_FORMAT_H

#include <stdint.h>
#include <stdbool.h>

/*
 * On-disk module format produced by tools/packer.py. The MCU never parses
 * ELF -- it only ever sees this flat layout:
 *
 *   [mdl_header_t][text][data][got][reloc_table]
 *
 * abi_ver must match the host's vtable version (host/host_api.h) exactly;
 * arch must match the target this firmware was built for.
 */

#define MDL_MAGIC 0x304C444Du /* 'MDL0', little-endian on disk */

typedef enum {
    MDL_ARCH_ARMV7M = 1,
    MDL_ARCH_RV32   = 2,
} mdl_arch_t;

typedef struct {
    uint32_t magic;
    uint16_t abi_ver;
    uint16_t arch;        /* mdl_arch_t */
    uint32_t crc32;       /* covers everything after this header */
    uint32_t text_size;
    uint32_t data_size;
    uint32_t bss_size;
    uint32_t got_off;     /* relative to payload start (i.e. after header) */
    uint32_t got_count;
    uint32_t init_off;    /* module_init() offset relative to text start */
    uint32_t reloc_off;
    uint32_t reloc_count;
} mdl_header_t;

/*
 * One entry per GOT slot that needs a runtime base address added at load
 * time. -msingle-pic-base routes ALL address-of-global-data through
 * r9 + GOT (r9 = runtime GOT base, loaded once before entering the
 * module) -- so loading is "walk the GOT, add one base address per slot"
 * -- not a general relocation engine.
 *
 * kind exists because empirically (verified against real
 * arm-none-eabi-gcc 12.2 output, see mdl/tests/modules/hello) a GOT slot
 * does NOT always hold the address of a *data* object: with
 * -mno-pic-data-is-text-relative, the compiler cannot assume .rodata
 * (string literals, const tables -- placed in the TEXT region, alongside
 * .text) is reachable via PC-relative addressing from .text, so it
 * routes .rodata addresses through the GOT too, exactly like a mutable
 * global. A GOT slot's original (link-time, base-address-0) value tells
 * you which: it falls either inside the linked .text+.rodata range or
 * inside the linked .data(+.got) range, never both. packer.py classifies
 * each slot by that range check and normalizes the stored value to an
 * in-blob offset (see mdl_header_t.got_off's comment); the MCU-side
 * loader then does exactly one of:
 *   MDL_RELOC_TEXT_BASE: *slot += module's runtime text-region base
 *   MDL_RELOC_DATA_BASE: *slot += module's runtime data-region base
 * with no further interpretation needed.
 */
typedef enum {
    MDL_RELOC_TEXT_BASE = 0,
    MDL_RELOC_DATA_BASE = 1,
} mdl_reloc_kind_t;

typedef struct {
    uint32_t got_offset; /* byte offset into the GOT area, itself always
                           * inside the data blob (see got_off) --
                           * regardless of which base `kind` says to add
                           * to the value stored there */
    uint8_t  kind;        /* mdl_reloc_kind_t */
    uint8_t  _reserved[3];
} mdl_reloc_t;

/*
 * Permission flags for one MPU/PMP-backed region. MDL_PERM_NONE means no
 * access at all, at any privilege level, regardless of privileged_only.
 */
typedef enum {
    MDL_PERM_NONE  = 0,
    MDL_PERM_READ  = 1u << 0,
    MDL_PERM_WRITE = 1u << 1,
    MDL_PERM_EXEC  = 1u << 2,
} mdl_perm_t;

/*
 * Region description passed to arch_setup_regions(). Callers order the
 * array from highest priority (index 0) to lowest priority (index n-1).
 * What "priority" maps to in hardware is entirely the arch layer's
 * business (ARMv7-M MPU: the highest region *number* wins an overlap;
 * RISC-V PMP: the lowest entry *number* wins) -- callers must not assume
 * either scheme, that asymmetry stays behind arch_setup_regions().
 */
typedef struct {
    void      *base;
    uint32_t   size;
    mdl_perm_t perm;
    bool       privileged_only; /* reserved for future host-private regions;
                                    v1's four arena regions are all reachable
                                    by unprivileged code when perm != NONE */
} mdl_region_t;

#endif /* MDL_FORMAT_H */
