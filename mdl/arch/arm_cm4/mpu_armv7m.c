#include "mpu_armv7m.h"
#include "arch_if.h"

/*
 * "cmsis_device.h" is a one-file indirection every project supplies its
 * own copy of (on its own -I include path, taking precedence over
 * anything of the same name elsewhere): it must define __FPU_PRESENT,
 * __MPU_PRESENT, __NVIC_PRIO_BITS and IRQn_Type, THEN #include
 * core_cm4.h itself (CMSIS-Core's standard Cortex-M4 header: MPU_Type,
 * SCB_Type, __DSB()/__ISB() intrinsics, register field masks) -- CMSIS
 * requires that ordering, core_cm4.h must never be included directly,
 * only via a device header, or the device-specific macros it depends on
 * won't exist yet.
 *
 * For a real AT32F435 target that one file is just
 * `#include "at32f435_437.h"` (Artery's own device header already does
 * the right thing) -- see mdl/tests/hil/at32f435_m0/inc/cmsis_device.h.
 * For the QEMU mps2-an386 target it's a handful of macros plus a direct
 * core_cm4.h include, since there's no vendor device header at all --
 * see mdl/tests/qemu/cmsdk_m4/inc/cmsis_device.h. This is what makes
 * arch/arm_cm4/ actually vendor-portable (any Cortex-M4, not just
 * AT32F435) rather than silently AT32-locked through a hardcoded
 * #include here.
 */
#include "cmsis_device.h"

/*
 * ARMv7-M MPU region sizes are powers of two, minimum 32 bytes, and the
 * SIZE field encodes actual_size = 2^(SIZE+1) bytes (so SIZE=4 -> 32B,
 * the architectural minimum). Every region size in the v1 arena layout
 * (mdl/linker/mdl_arena.ld) already satisfies this by construction; this
 * just encodes it, it does not validate it. A caller that passes a
 * non-power-of-two size gets a rounded-down/incorrect field here, which
 * will surface immediately as spurious faults -- there is no silent
 * memory-safety failure mode, just a loud wrong one.
 */
static uint32_t size_field(uint32_t bytes)
{
    uint32_t field = 0;
    uint32_t sz = 32;

    while (sz < bytes) {
        sz <<= 1;
        field++;
    }
    return field + 4;
}

static void perm_to_ap_xn(mdl_perm_t perm, uint32_t *ap, uint32_t *xn)
{
    if (perm == MDL_PERM_NONE) {
        *ap = 0x0; /* no access at all, any privilege level */
        *xn = 1;
        return;
    }

    *xn = (perm & MDL_PERM_EXEC) ? 0u : 1u;
    *ap = (perm & MDL_PERM_WRITE) ? 0x3u /* RW, priv + unpriv */
                                   : 0x6u; /* RO, priv + unpriv */
}

static void mpu_program_region(uint32_t region_no, const mdl_region_t *r)
{
    uint32_t ap, xn;
    perm_to_ap_xn(r->perm, &ap, &xn);

    MPU->RNR  = region_no;
    MPU->RBAR = ((uint32_t)r->base & MPU_RBAR_ADDR_Msk);
    MPU->RASR =
        (ap << MPU_RASR_AP_Pos) |
        (xn << MPU_RASR_XN_Pos) |
        (1u << MPU_RASR_S_Pos) |  /* shareable: other bus masters (DMA,
                                      USB) touch this SRAM too; harmless
                                      no-op for coherency since M4 has no
                                      cache, kept for correctness/clarity */
        (1u << MPU_RASR_C_Pos) |
        (1u << MPU_RASR_B_Pos) |
        (size_field(r->size) << MPU_RASR_SIZE_Pos) |
        (1u << MPU_RASR_ENABLE_Pos);
}

void arch_setup_regions(const mdl_region_t *rs, size_t n)
{
    MPU->CTRL = 0;
    __DSB();
    __ISB();

    for (size_t i = 0; i < n; i++) {
        /* rs[0] is highest priority per the arch_if.h contract. ARMv7-M:
         * on an address overlap, the highest-*numbered* region wins, so
         * priority order maps onto descending region numbers here. */
        uint32_t region_no = MDL_MPU_REGION_BASE + (uint32_t)(n - 1 - i);
        mpu_program_region(region_no, &rs[i]);
    }

    /*
     * PRIVDEFENA=1, deliberately -- NOT 0, despite "lock everything down"
     * intuition suggesting otherwise. Per the ARMv7-M ARM, PRIVDEFENA=0
     * means ANY access outside the regions programmed above faults,
     * *including privileged accesses*: the host firmware's own code/data
     * in flash/RAM, and the FreeRTOS kernel itself, live at addresses
     * that are not one of the four module-arena regions above. With
     * PRIVDEFENA=0 the board would fault on its own first instruction
     * after this function returns. Unprivileged accesses to the
     * background map are ALWAYS denied regardless of this bit -- that,
     * not PRIVDEFENA, is the actual isolation mechanism for module
     * tasks, enforced per-task by FreeRTOS-MPU's xTaskCreateRestricted()
     * regions once M2 lands. PRIVDEFENA only controls the privileged
     * fallback that the host needs to keep running.
     */
    MPU->CTRL = MPU_CTRL_ENABLE_Msk | MPU_CTRL_PRIVDEFENA_Msk | MPU_CTRL_HFNMIENA_Msk;
    __DSB();
    __ISB();

    /*
     * Enable the MemManage exception itself. Without this bit a region
     * violation is not "a MemManage fault that our handler sees" -- the
     * MemManage exception is disabled, so the fault escalates straight to
     * HardFault and MemManage_Handler() in fault_arm.c never runs at all.
     *
     * FreeRTOS-MPU's own prvSetupMPU() sets this too (third_party/
     * freertos_mpu_port/port.c, portNVIC_MEM_FAULT_ENABLE), but that only
     * runs from xPortStartScheduler() -- i.e. only on the M2+ targets.
     * M0/M1 are bare-metal with no scheduler, so nothing enabled it there
     * and every arena violation silently became a HardFault. Setting it
     * here, next to the MPU enable it belongs with, covers both cases;
     * port.c's later |= of the same bit is then a harmless no-op.
     */
    SCB->SHCSR |= SCB_SHCSR_MEMFAULTENA_Msk;
    __DSB();
    __ISB();
}

void arch_code_sync(void *addr, size_t len)
{
    (void)addr;
    (void)len;
    /* Cortex-M4: no I/D cache to manage -- see the platform note in
     * mpu_armv7m.h. A pipeline/prefetch flush via DSB+ISB is the whole
     * story on this core. */
    __DSB();
    __ISB();
}
