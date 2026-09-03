# FreeRTOS-Kernel MPU port (vendored)

`port.c` and `portmacro.h` in this directory are fetched verbatim from:

    https://raw.githubusercontent.com/FreeRTOS/FreeRTOS-Kernel/V10.4.3/portable/GCC/ARM_CM4_MPU/port.c
    https://raw.githubusercontent.com/FreeRTOS/FreeRTOS-Kernel/V10.4.3/portable/GCC/ARM_CM4_MPU/portmacro.h

**Why:** Artery's AT32F435_437_Firmware_Library_V2.2.6 bundles FreeRTOS-Kernel
V10.4.3 (`middlewares/freertos/source/`, confirmed via
`tskKERNEL_VERSION_NUMBER` in `include/task.h`) but only ships the
non-MPU `portable/GCC/ARM_CM4F` port. This project requires the MPU port
(`portable/GCC/ARM_CM4_MPU`) per its own design -- module tasks must run
as `xTaskCreateRestricted()`-created unprivileged tasks. Rather than
hand-write MPU context-switch/SVC-privilege-escalation code (error-prone,
and the task's own instructions say not to), these two files were pulled
from the **same V10.4.3 tag** as the rest of the bundled kernel, so
`port.c`/`portmacro.h` stay in lockstep with the vendor's
`tasks.c`/`queue.c`/`list.c`/`mpu_wrappers.c`/etc. -- no version-skew risk.

**Unmodified.** No AT32-specific changes were needed: this port only
touches Cortex-M4 core registers (NVIC, SCB, MPU, SysTick) via the
standard portmacro.h intrinsics, nothing AT32-peripheral-specific.
AT32-side integration (interrupt priority bits, MPU region count, the
`__privileged_functions_start__`-style linker symbols this port expects)
lives in `mdl/tests/hil/at32f435_m2/` instead -- see that project's
`FreeRTOSConfig.h` and linker script.

**Region budget this port fixes in stone** (see `portmacro.h`):
`portUNPRIVILEGED_FLASH_REGION`=0, `portPRIVILEGED_FLASH_REGION`=1,
`portPRIVILEGED_RAM_REGION`=2, `portGENERAL_PERIPHERALS_REGION`=3,
`portSTACK_REGION`=4, `portFIRST_CONFIGURABLE_REGION`=5. On an 8-region
ARMv7-M MPU (AT32F435/437: confirmed 8 regions) that leaves exactly 3
configurable regions per task (5, 6, 7) -- matching the spec's "每任务
最多 3 个可配置 region" exactly, and confirming (not assuming) why the
module's text/data/heap+stack sandbox needs to fit in 3 slots, not 4 --
see mdl/core/sandbox.c's M2 comment on how the guard region was folded
into the heap+stack region via ARMv7-M subregion-disable instead of
spending a 4th slot on it.
