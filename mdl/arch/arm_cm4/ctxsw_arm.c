#include "arch_if.h"

/*
 * Unprivileged entry into a module task. Not implemented yet -- lands in
 * M2 together with xTaskCreateRestricted() wiring and the SVC vtable.
 * Declared now so the arch_if.h contract is fully satisfied and M0 links
 * cleanly against it.
 *
 * M2 note: r9 (the -msingle-pic-base PIC base register) must be loaded
 * from got_base before entry and treated as task-owned state from then
 * on. Host code is not built -fPIC, so host C code never touches r9 --
 * but interrupt/exception entry and FreeRTOS's own context switch must
 * still save/restore it across a module task like any other callee-saved
 * register, or a module's globals silently resolve through the wrong
 * base after any preemption.
 */
void arch_enter_unprivileged(void *entry, void *stack, void *got_base)
{
    (void)entry;
    (void)stack;
    (void)got_base;
    /* TODO(M2) */
}
