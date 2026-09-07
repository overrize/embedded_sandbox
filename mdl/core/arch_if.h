#ifndef MDL_ARCH_IF_H
#define MDL_ARCH_IF_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "mdl_format.h"

/*
 * Everything architecture-specific lives behind these five functions.
 * core/ must never include an arch header or touch an __ARM / CMSIS /
 * RISC-V symbol directly -- if core/ needs new arch behavior, it grows
 * this interface instead of reaching around it.
 *
 * module_t is intentionally opaque here: arch implementations that need
 * it (currently none do -- arch_apply_relocs takes it only to satisfy
 * this exact signature) get the full definition from core/registry.h,
 * which arch/ files are free to include. arch_if.h itself never does,
 * so core/ headers stay arch-clean.
 */
typedef struct module module_t;

/* Apply the module's GOT-base relocations. tbl/n describe entries read
 * verbatim from the packed module's reloc table (mdl_reloc_t, M1). */
void arch_apply_relocs(module_t *m, const mdl_reloc_t *tbl, size_t n);

/* Program the hardware's memory-protection unit with exactly these
 * regions and nothing else reachable outside them (background/default
 * access for unprivileged code is always off; see the arch implementation
 * for how privileged default access is handled). rs is ordered highest
 * priority first; see mdl_region_t's comment in mdl_format.h. */
void arch_setup_regions(const mdl_region_t *rs, size_t n);

/* Drop to unprivileged execution at `entry`, running on `stack`, with the
 * PIC base register (r9 on ARM) loaded from `got_base`. Does not return.
 * Implemented in M2 alongside xTaskCreateRestricted() wiring. */
void arch_enter_unprivileged(void *entry, void *stack, void *got_base);

/* Make code written into addr..addr+len visible to the fetch unit before
 * it is executed. On Cortex-M4 (no I/D cache) this is __DSB()+__ISB() and
 * nothing else -- see the platform note in arch/arm_cm4/mpu_armv7m.h. */
void arch_code_sync(void *addr, size_t len);

/* True if pc falls in [lo, hi). Used to classify a faulting PC as
 * "inside some module's text" vs. "somewhere in the host" during fault
 * handling (M3). */
bool arch_pc_in_range(uintptr_t pc, uintptr_t lo, uintptr_t hi);

/*
 * M1-only bridge, not one of the five interfaces above: calls
 * `entry(arg0)` with the PIC base register (r9 on ARM) loaded from
 * `got_base` first, returning entry's return value. Unlike
 * arch_enter_unprivileged() this DOES return, and does not touch
 * privilege level or the stack pointer -- M1's loader calls module_init()
 * directly from the loader's own (privileged) call stack, so all this
 * needs to do is get r9 right around an otherwise-ordinary call.
 *
 * M2 replaces the loader's use of this with arch_enter_unprivileged()
 * (a real task entry, privilege drop, dedicated PSP stack); this stays
 * available for anything that still needs a same-context, same-privilege
 * call into module code with r9 set correctly (e.g. re-invoking a
 * module-supplied callback from privileged context).
 */
int arch_call_privileged(void *entry, void *got_base, const void *arg0);

/*
 * Same, for the three-argument module_cmd(host, argc, argv) entry point
 * [ABI v2]. A separate function rather than a varargs one because the
 * whole point here is precise control over which argument lands in
 * which register, and over r9.
 */
/* Two-argument variant, for module_event(host, evt) [ABI v4]. */
int arch_call_module2(void *entry, void *got_base,
                       const void *a0, const void *a1);

int arch_call_module3(void *entry, void *got_base,
                       const void *a0, int a1, const void *a2);

#endif /* MDL_ARCH_IF_H */
