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

/* Longest console command name a module may export, NUL included. */
#define MDL_CMD_NAME_MAX 16u

/* Name of the MDL itself, for `status` -- what a person calls the thing
 * currently loaded. Distinct from cmd_name, which is the console command
 * an MDL may export; most MDLs have a name and no command. */
#define MDL_NAME_MAX 16u

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

    /* ---- ABI v2 additions (append-only, per maintain.md D3) ---- */

    /* module_cmd() offset into text, Thumb bit set exactly like
     * init_off. 0 means the module exports no console command, which is
     * the normal case -- a module is not required to be interactive. */
    uint32_t cmd_off;

    /* Declared hardware claims: mdl_res_t[res_count] at res_off,
     * relative to payload start. res_count == 0 means the module
     * claims nothing, and with the runtime check in host_api.c that
     * means it can touch no GPIO at all -- silence is not permission. */
    uint32_t res_off;
    uint32_t res_count;

    /* Console command name, NUL-terminated; all zero when cmd_off is 0.
     * Fixed-width rather than an offset because it is bounded, tiny, and
     * the loader wants it before it has decided to copy any payload. */
    char cmd_name[MDL_CMD_NAME_MAX];

    /* Human-facing name of this MDL, NUL-terminated. The packer fills it
     * from the source directory unless MDL_MODULE_NAME() overrides, so
     * `status` can say which one is loaded without every author having
     * to remember to declare anything. */
    char name[MDL_NAME_MAX];
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
 * Declared hardware claims (ABI v2).
 *
 * The point of making a module SAY what it touches is that the host can
 * refuse the load instead of discovering the conflict as a symptom. The
 * concrete case this was built for: the host blinks LEDB (whitelist pin
 * 0) once a second as its 'firmware alive' signal. A module driving the
 * same pin does not crash anything -- it quietly turns that indicator
 * into a lie, which is worse, because the one signal you use to decide
 * whether the board is alive is the one that has stopped meaning
 * anything.
 *
 * Enforced twice, deliberately (see host_api.c):
 *   load time -- a claim the host already owns rejects the whole image,
 *                before a single byte is copied into the arena;
 *   call time -- gpio_set/gpio_get refuse a pin this module did not
 *                declare. Without the second check a module could
 *                declare pin 1 and drive pin 0 anyway, and the manifest
 *                would be documentation rather than a constraint.
 */
typedef enum {
    MDL_RES_KIND_NONE = 0,
    MDL_RES_KIND_GPIO = 1, /* id = index into the host GPIO whitelist */
} mdl_res_kind_t;

typedef struct {
    uint8_t kind; /* mdl_res_kind_t */
    uint8_t id;
    uint8_t _reserved[2];
} mdl_res_t;

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
