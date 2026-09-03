#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

/*
 * FreeRTOSConfig.h for AT32F435xG + the vendored FreeRTOS-Kernel V10.4.3
 * MPU port (mdl/third_party/freertos_mpu_port/). configENFORCE_SYSTEM_
 * CALLS_FROM_KERNEL_ONLY=1 is the load-bearing setting here -- without
 * it, ANY code (including a malicious/buggy module) can execute
 * `svc #2` directly and self-elevate to privileged, defeating the whole
 * sandbox. With it, portRAISE_PRIVILEGE()'s SVC handler only actually
 * elevates when the calling PC falls inside the linker-defined
 * __syscalls_flash_start__/__syscalls_flash_end__ range -- see this
 * project's linker script for what's placed there (FreeRTOS's own
 * mpu_wrappers.o, plus mdl/host/host_api.c's SVC-gate functions).
 */

#include <stdint.h>
/*
 * AT32's system_at32f435_437.h does NOT declare a real `SystemCoreClock`
 * symbol (unlike ST's CMSIS convention) -- it #defines SystemCoreClock
 * as a macro aliasing the real (lowercase) symbol `system_core_clock`.
 * Declaring our own `extern uint32_t SystemCoreClock` collided with
 * that macro expansion (redeclaring the real symbol under mismatched
 * apparent types depending on include order) -- declare the real name
 * directly instead and never spell `SystemCoreClock` in this file.
 * The type must be `unsigned int`, not `uint32_t`: on this
 * arm-none-eabi/newlib target uint32_t is `unsigned long`, a distinct
 * type from `unsigned int` despite identical width -- redeclaring an
 * extern under the "wrong" of the two is a real conflicting-types error,
 * not just a style nit. */
extern unsigned int system_core_clock; /* set by SystemInit(), system_at32f435_437.c */

#define configUSE_PREEMPTION                     1
#define configUSE_PORT_OPTIMISED_TASK_SELECTION  0
#define configUSE_TICKLESS_IDLE                  0
#define configCPU_CLOCK_HZ                       (system_core_clock)
#define configTICK_RATE_HZ                       (1000) /* 1 tick == 1ms, so xTaskGetTickCount() doubles as uptime_ms() */
#define configMAX_PRIORITIES                     (8)
#define configMINIMAL_STACK_SIZE                 (128) /* words, for the idle task */
#define configMAX_TASK_NAME_LEN                  (12)
#define configUSE_16_BIT_TICKS                   0
#define configIDLE_SHOULD_YIELD                  1
#define configUSE_MUTEXES                        1
#define configUSE_RECURSIVE_MUTEXES              0
#define configUSE_COUNTING_SEMAPHORES            1
#define configQUEUE_REGISTRY_SIZE                4
#define configUSE_QUEUE_SETS                     0
#define configUSE_TIME_SLICING                   1
#define configUSE_NEWLIB_REENTRANT               0
#define configSTACK_DEPTH_TYPE                   uint16_t

#define configSUPPORT_STATIC_ALLOCATION           1
#define configSUPPORT_DYNAMIC_ALLOCATION           1
#define configTOTAL_HEAP_SIZE                      (16 * 1024) /* kernel objects only -- NEVER the module's own
                                                                  * pool (mdl/host/host_api.c carves that out of
                                                                  * the module's own arena region instead) */
#define configAPPLICATION_ALLOCATED_HEAP           0

/* ---- MPU-specific ---- */
#define configTOTAL_MPU_REGIONS                    8 /* ARMv7-M standard for Cortex-M4; NOT verified against
                                                        * MPU_TYPE.DREGION on real AT32F435 silicon in this
                                                        * sandbox (no hardware access) -- verify before trusting
                                                        * this on a board variant you haven't checked. */
#define configENFORCE_SYSTEM_CALLS_FROM_KERNEL_ONLY 1
#define configTEX_S_C_B_FLASH                      (0x07UL) /* AT32 SRAM/flash: treat as normal, cacheable-ish
                                                                * bits (irrelevant on M4 -- no cache -- kept for
                                                                * port.c's MPU_TEX_S_C_B_FLASH macro to have a value) */
#define configTEX_S_C_B_SRAM                       (0x07UL)
#define configENABLE_MPU                           1
#define configENABLE_FPU                           1
#define configENABLE_TRUSTZONE                     0
#define configRUN_FREERTOS_SECURE_ONLY             0

/* ---- hooks ---- */
#define configUSE_IDLE_HOOK                      0
#define configUSE_TICK_HOOK                      0
#define configCHECK_FOR_STACK_OVERFLOW            2 /* belt-and-suspenders alongside the MPU guard subregion --
                                                       * see sandbox.c's M2 comment on why the guard is a disabled
                                                       * MPU subregion now, not a 4th region */
#define configUSE_MALLOC_FAILED_HOOK              1
#define configUSE_DAEMON_TASK_STARTUP_HOOK        0

#define configGENERATE_RUN_TIME_STATS             0
#define configUSE_TRACE_FACILITY                  0
#define configUSE_STATS_FORMATTING_FUNCTIONS      0

#define configUSE_CO_ROUTINES                    0
#define configMAX_CO_ROUTINE_PRIORITIES          (2)

#define configUSE_TIMERS                         1
#define configTIMER_TASK_PRIORITY                (configMAX_PRIORITIES - 2) /* high -- a system task, per the
                                                                               * spec's priority-ordering rule */
#define configTIMER_QUEUE_LENGTH                 10
#define configTIMER_TASK_STACK_DEPTH             (configMINIMAL_STACK_SIZE * 2)

/* Priority band reservation (spec: "模块任务优先级必须低于所有系统任务
 * 和通信任务"). Module tasks always use MDL_MODULE_TASK_PRIORITY (see
 * mdl/core/module_task.h); every host-side task must use a priority
 * strictly greater than that. */
#define MDL_MODULE_TASK_PRIORITY                 (1)
#define MDL_LOADER_TASK_PRIORITY                 (configMAX_PRIORITIES - 3)
#define MDL_USB_TASK_PRIORITY                    (configMAX_PRIORITIES - 2)

#define INCLUDE_vTaskPrioritySet                 1
#define INCLUDE_uxTaskPriorityGet                1
#define INCLUDE_vTaskDelete                      1
#define INCLUDE_vTaskSuspend                     1
#define INCLUDE_vTaskDelayUntil                  1
#define INCLUDE_vTaskDelay                       1
#define INCLUDE_xTaskGetSchedulerState           1
#define INCLUDE_xTaskGetCurrentTaskHandle        1
#define INCLUDE_uxTaskGetStackHighWaterMark      1
#define INCLUDE_xTaskGetIdleTaskHandle           0
#define INCLUDE_eTaskGetState                    1
#define INCLUDE_xEventGroupSetBitFromISR         0
#define INCLUDE_xTimerPendFunctionCall           1
#define INCLUDE_xTaskAbortDelay                  0
#define INCLUDE_xTaskGetHandle                   0
#define INCLUDE_xTaskResumeFromISR               1

#define configKERNEL_INTERRUPT_PRIORITY          (7 << 4)  /* AT32F435: __NVIC_PRIO_BITS == 4, so priority
                                                              * values occupy the top 4 bits of the 8-bit field */
#define configMAX_SYSCALL_INTERRUPT_PRIORITY     (5 << 4)  /* ISRs at this priority or higher (numerically
                                                              * lower) must NEVER call a FreeRTOS *FromISR API */
#define configPRIO_BITS                          4

#define configASSERT_DEFINED                     1
void mdl_freertos_assert_failed(const char *file, int line);
#define configASSERT(x)  if ((x) == 0) { mdl_freertos_assert_failed(__FILE__, __LINE__); }

/* Map the standard CMSIS vector-table names (already weakly aliased to
 * Default_Handler by the vendor startup .s) onto the port's actual
 * handler names -- pure token substitution, no port.c changes needed.
 * MemManage_Handler is NOT remapped: that stays mdl/arch/arm_cm4/
 * fault_arm.c's handler, untouched by FreeRTOS. */
#define vPortSVCHandler       SVC_Handler
#define xPortPendSVHandler    PendSV_Handler
#define xPortSysTickHandler   SysTick_Handler

#endif /* FREERTOS_CONFIG_H */
