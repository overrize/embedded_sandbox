#ifndef MDL_SANDBOX_H
#define MDL_SANDBOX_H

/*
 * Records the fixed v1 arena bounds (from the linker symbols in
 * mdl/linker/mdl_arena.ld) into g_mdl_slot. Programs no hardware.
 *
 * THIS IS THE ONE TO CALL ON THE RTOS TARGETS (M2+). There, the module's
 * MPU regions are FreeRTOS's business, not ours: they are programmed
 * per-task from xTaskCreateRestricted()'s xRegions[] and reloaded on
 * every context switch, so anything sandbox_init() writes into regions
 * 4..7 is overwritten before a module ever runs. What those targets
 * still need is the bookkeeping -- without it mdl_load() copies the
 * module to text_lo == 0.
 *
 * Call once at boot, after registry_init() and before any module load.
 */
void sandbox_bounds_init(void);

/*
 * sandbox_bounds_init() PLUS programming the four arena regions into the
 * MPU directly (via arch_setup_regions()), which also enables the
 * MemManage exception.
 *
 * For the bare-metal targets (M0/M1) that have no RTOS to own the MPU.
 * On an RTOS target this is not wrong so much as pointless for the
 * region half -- see sandbox_bounds_init() above.
 */
void sandbox_init(void);

#endif /* MDL_SANDBOX_H */
