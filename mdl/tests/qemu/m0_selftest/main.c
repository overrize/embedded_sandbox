/*
 * mdl/tests/qemu/m0_selftest/main.c
 *
 * M0 acceptance check: MPU regions program correctly, and an
 * out-of-bounds access reliably takes a MemManage fault instead of
 * silently succeeding or corrupting memory.
 *
 * NOT WIRED TO A BUILD YET. This sandbox has no arm-none-eabi-gcc,
 * qemu-system-arm, or git available, so this file has been written
 * carefully but never compiled, linked, or run. Before trusting it:
 *
 *   - supply a startup file / vector table that installs MemManage_Handler
 *     at the correct vector and calls SystemInit() + main() -- the Artery
 *     BSP for real AT32F435 hardware, or a minimal CMSDK-M4 startup for
 *     `qemu-system-arm -M mps2-an386`
 *   - supply core_cm4.h (from the vendor's CMSIS package)
 *   - INCLUDE mdl/linker/mdl_arena.ld from that target's linker script,
 *     with the .mdl_arena section's RAM region matching your memory map
 *   - build, then single-step under QEMU or a debugger and confirm
 *     g_mdl_last_fault.occurred flips to true after the guard-region touch
 *     below, and that execution never reaches the bkpt loop
 *
 * Why the guard region specifically, and not some address past the
 * arena: MPU_CTRL.PRIVDEFENA is intentionally left enabled (see the
 * comment in arch/arm_cm4/mpu_armv7m.c) so the host keeps default-map
 * access to everything it doesn't explicitly carve out. This whole test
 * runs privileged (no unprivileged module task exists until M2), so an
 * address outside the arena entirely would resolve through that default
 * map and just work -- it would prove nothing. The guard region is the
 * one place in the v1 layout with MDL_PERM_NONE, which blocks access at
 * any privilege level; touching it is the only way at this stage to
 * prove the MPU is actually programmed and enforced, not just configured.
 */
#include <stdint.h>
#include "sandbox.h"
#include "registry.h"

volatile uint8_t g_probe_readback;

int main(void)
{
    registry_init();
    sandbox_init();

    volatile uint8_t *guard = (volatile uint8_t *)g_mdl_slot.guard_lo;
    g_probe_readback = *guard; /* expected: MemManage fault, never returns */

    /* Reaching here means the guard region did not actually block the
     * access -- isolation is broken. Trap loudly instead of continuing. */
    for (;;) {
        __asm volatile ("bkpt #0");
    }
}
