#ifndef MDL_MPU_ARMV7M_H
#define MDL_MPU_ARMV7M_H

/*
 * ARMv7-M MPU driver backing arch_setup_regions() / arch_code_sync() /
 * arch_pc_in_range() from core/arch_if.h.
 *
 * PLATFORM NOTE (read before porting to Cortex-M7): the Cortex-M4 has no
 * L1 instruction or data cache. Code copied into SRAM is visible to the
 * fetch unit as soon as the pipeline is flushed, so making it safe to
 * execute only requires __DSB() + __ISB() (see arch_code_sync() in
 * fault_arm.c / mpu_armv7m.c) -- there is no SCB->CleanDCache /
 * InvalidateICache step, because there is no cache to clean or
 * invalidate. On an M7 port, arch_code_sync() is the one place in this
 * whole directory that changes: it would need
 * SCB_CleanDCache_by_Addr(addr, len) before the DSB and
 * SCB_InvalidateICache_by_Addr(addr, len) before the ISB. Nothing else
 * here is cache-related, and nothing outside arch/ should ever need to
 * know this distinction exists.
 *
 * Regions 0..MDL_MPU_REGION_BASE-1 are left free for FreeRTOS-MPU's own
 * kernel regions once the M2 FreeRTOS-MPU (GCC/ARM_CM4_MPU) port lands;
 * the four module-arena regions programmed by arch_setup_regions() start
 * at MDL_MPU_REGION_BASE and count down from there (see the priority ->
 * region-number mapping comment in mpu_armv7m.c).
 */

#define MDL_MPU_REGION_BASE 4u

#endif /* MDL_MPU_ARMV7M_H */
