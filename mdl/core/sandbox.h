#ifndef MDL_SANDBOX_H
#define MDL_SANDBOX_H

/*
 * Programs the fixed v1 arena layout into the MPU (via arch_setup_regions())
 * and records the resulting bounds into g_mdl_slot (registry.h). Call once
 * at boot, after registry_init() and before any module is loaded.
 */
void sandbox_init(void);

#endif /* MDL_SANDBOX_H */
