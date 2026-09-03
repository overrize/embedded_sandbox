/*
 * AT32F435 target's cmsis_device.h -- see the comment in
 * mdl/arch/arm_cm4/mpu_armv7m.c for what this indirection is for.
 * Artery's own device header already does the right thing (defines
 * __FPU_PRESENT/__MPU_PRESENT/__NVIC_PRIO_BITS/IRQn_Type before pulling
 * in core_cm4.h), so this file is a pure pass-through.
 */
#include "at32f435_437.h"
