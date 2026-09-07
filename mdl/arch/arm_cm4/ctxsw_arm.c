#include "arch_if.h"

/*
 * arch_call_privileged(entry, got_base, arg0): call `((int(*)(const void*))entry)(arg0)`
 * with r9 = got_base for the duration of the call, from the current
 * (privileged) stack -- see arch_if.h's comment on why this exists
 * alongside arch_enter_unprivileged().
 *
 * AAPCS puts arg0 in r0 and the (C-level) first two parameters `entry`,
 * `got_base` in r0/r1 on entry to THIS function -- naked, so we do our
 * own register shuffling rather than trust the compiler's prologue,
 * since we need r9 (a callee-saved/PIC-base register the compiler
 * otherwise owns) to hold a specific value only for the duration of the
 * nested call, and restored after.
 */
__attribute__((naked)) int arch_call_privileged(void *entry, void *got_base, const void *arg0)
{
    /* naked: entry/got_base/arg0 are consumed purely through the AAPCS
     * r0/r1/r2 register convention inside the asm block below, never
     * through a normal C-level read -- these casts are silencing
     * -Wunused-parameter without emitting any code (a naked function
     * has no prologue to spend it in). */
    (void)entry;
    (void)got_base;
    (void)arg0;
    __asm volatile (
        "push  {r4, r9, lr}   \n" /* r4: scratch for entry; r9: caller's PIC base, saved/restored */
        "mov   r4, r0         \n" /* r4 = entry */
        "mov   r9, r1         \n" /* r9 = got_base, for the callee's r9-relative global access */
        "mov   r0, r2         \n" /* r0 = arg0 (module_init's sole argument, per AAPCS) */
        "blx   r4             \n" /* r0 = entry(arg0); r0 already holds module's return value after */
        "pop   {r4, r9, pc}   \n"
    );
}

/*
 * arch_call_module2(entry, got_base, a0, a1): call
 * `((int(*)(const void*, const void*))entry)(a0, a1)` with r9 = got_base.
 * For module_event(host, evt) [ABI v4]. Both arguments are in registers,
 * so unlike arch_call_module3() there is no stacked-argument offset to
 * get wrong.
 */
__attribute__((naked)) int arch_call_module2(void *entry, void *got_base,
                                              const void *a0, const void *a1)
{
    (void)entry;
    (void)got_base;
    (void)a0;
    (void)a1;
    __asm volatile (
        "push  {r4, r9, lr}   \n"
        "mov   r4, r0         \n" /* r4 = entry */
        "mov   r9, r1         \n" /* r9 = got_base */
        "mov   r0, r2         \n" /* r0 = a0 (host) */
        "mov   r1, r3         \n" /* r1 = a1 (event) */
        "blx   r4             \n"
        "pop   {r4, r9, pc}   \n"
    );
}

/*
 * arch_call_module3(entry, got_base, a0, a1, a2): call
 * `((int(*)(const void*, int, const void*))entry)(a0, a1, a2)` with
 * r9 = got_base, same contract as arch_call_privileged() above but for
 * the three-argument module_cmd(host, argc, argv) entry point [ABI v2].
 *
 * a2 arrives on the stack, not in a register: AAPCS passes only the
 * first four arguments in r0-r3, and this function has five. After the
 * push below moves sp down by 12 bytes, that incoming word sits at
 * [sp, #12] -- get the offset wrong and the module receives a garbage
 * argv pointer, which the MPU then rejects in a place that looks
 * nothing like the cause.
 */
__attribute__((naked)) int arch_call_module3(void *entry, void *got_base,
                                              const void *a0, int a1, const void *a2)
{
    (void)entry;
    (void)got_base;
    (void)a0;
    (void)a1;
    (void)a2;
    __asm volatile (
        "push  {r4, r9, lr}   \n" /* sp -= 12, so the stacked 5th arg moves to [sp,#12] */
        "ldr   r4, [sp, #12]  \n" /* r4 = a2 (argv) */
        "mov   r12, r0        \n" /* r12 = entry; r0-r2 are about to be overwritten */
        "mov   r9, r1         \n" /* r9 = got_base, for r9-relative global access */
        "mov   r0, r2         \n" /* r0 = a0 (host) */
        "mov   r1, r3         \n" /* r1 = a1 (argc) */
        "mov   r2, r4         \n" /* r2 = a2 (argv) */
        "blx   r12            \n"
        "pop   {r4, r9, pc}   \n"
    );
}

/*
 * Unprivileged entry into a module task. Not implemented yet -- lands in
 * M2 together with xTaskCreateRestricted() wiring and the SVC vtable.
 * Declared now so the arch_if.h contract is fully satisfied and M0/M1
 * link cleanly against it.
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
