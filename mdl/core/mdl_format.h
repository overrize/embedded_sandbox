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

/* How many events the host can hold for one MDL, and the most an MDL may
 * ask for with MDL_MODULE_EVENTS(). Shared because the packer checks a
 * declaration against it before the image ever reaches a device -- that
 * static half is the part of 'will it overflow' that can honestly be
 * decided early. */
#define MDL_EVT_QUEUE_MAX 16u

/* Most resource claims one MDL may declare. */
#define MDL_MAX_RES 8u

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

    /* ---- ABI v4: events ---- */

    /* module_event() offset into text, Thumb bit set; 0 = this MDL takes
     * no events and the host starts no resident task for it. */
    uint32_t evt_off;

    /* What the MDL says it needs. evt_queue_depth is checked against the
     * host's budget AT PACK TIME -- a static number against a static
     * limit, which is the part that can honestly be decided early.
     *
     * evt_rate_hz cannot be: how fast a button is pressed is a fact about
     * the world, not about the image, so no amount of packing-time
     * analysis can prove a queue will not overflow. It is recorded as a
     * CONTRACT instead -- the host measures the real rate and, when it
     * exceeds this, reports 'declared N/s, saw M/s' rather than a
     * mysterious loss of events. */
    uint16_t evt_queue_depth;
    uint16_t evt_rate_hz;
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
    MDL_RES_KIND_NONE  = 0,
    MDL_RES_KIND_GPIO  = 1, /* id = index into the host GPIO whitelist */
    MDL_RES_KIND_I2C   = 2, /* id = instance, e.g. 1 for I2C1  */
    MDL_RES_KIND_UART  = 3, /* id = instance, e.g. 2 for USART2 */
    MDL_RES_KIND_TIMER = 4, /* id = instance, e.g. 3 for TMR3   */
} mdl_res_kind_t;

/*
 * A physical pin, as port<<4 | number (PA0 = 0x00, PB7 = 0x17, ...).
 *
 * Conflict detection has to happen in THIS space, not in the space of
 * claim names. "I2C1" and "GPIO 2" look unrelated and can be the same
 * copper: on this board USART2_RX and the SW3 button are both PA3.
 * Comparing names would miss it; comparing pins cannot.
 */
#define MDL_PIN(port, num) ((uint8_t)(((port) << 4) | (num)))
#define MDL_PIN_PORT(p)    ((uint8_t)((p) >> 4))
#define MDL_PIN_NUM(p)     ((uint8_t)((p) & 0x0Fu))

/* Edge selection for a GPIO claim that also wants interrupts [ABI v4].
 * Lives in a byte that was already reserved, so the record keeps its
 * size and older images stay readable. */
typedef enum {
    MDL_EDGE_NONE    = 0, /* plain GPIO, no interrupt */
    MDL_EDGE_RISING  = 1,
    MDL_EDGE_FALLING = 2,
    MDL_EDGE_BOTH    = 3,
} mdl_edge_t;

typedef struct {
    uint8_t kind;  /* mdl_res_kind_t */
    uint8_t id;
    uint8_t edge;  /* mdl_edge_t; GPIO only, MDL_EDGE_NONE elsewhere */
    uint8_t _reserved;
} mdl_res_t;

/*
 * One event delivered to an MDL's module_event() [ABI v4].
 *
 * WHY `coalesced` EXISTS. When events arrive faster than the MDL drains
 * them, same-source events are merged rather than dropped. That is the
 * right default for STATE-like sources -- a pin level, where only the
 * latest value matters -- and quietly wrong for COUNT-like ones. Three
 * quick presses merged into one is fine for a lamp and is silent data
 * corruption for an encoder or a pulse counter.
 *
 * Carrying the merge count costs two bytes and removes the ambiguity
 * entirely: a handler that only wants the current state ignores it, and
 * one that is counting can recover the truth. Mitigation that hides how
 * much it hid is not mitigation, it is a later bug.
 *
 * `lost` is different and worse: it counts events the host could not
 * even merge (the queue was full of OTHER sources). That is real loss,
 * it is reported through `status`, and it means the MDL is not keeping
 * up with what it declared.
 */
typedef enum {
    MDL_EVT_NONE    = 0,
    MDL_EVT_GPIO    = 1, /* id = whitelist pin, payload = level at capture */
    MDL_EVT_CONSOLE = 2, /* a console command; see module_cmd() */
} mdl_evt_source_t;

typedef struct {
    uint8_t  source;    /* mdl_evt_source_t */
    uint8_t  id;
    uint16_t coalesced; /* how many merged into this one; 1 = none merged */
    uint32_t payload;
    uint32_t tick_ms;   /* when the FIRST of the merged events was captured */

    /* DWT cycle count at the moment the ISR captured this event.
     *
     * Milliseconds are useless for the number that matters here: the gap
     * between the interrupt firing and the MDL's handler running is a few
     * microseconds, and tick_ms cannot see it. Subtracting this from
     * host->cycles() at the top of module_event() measures the sandbox's
     * actual dispatch cost, in cycles, from inside the MDL. */
    uint32_t cycles;
} mdl_event_t;

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
