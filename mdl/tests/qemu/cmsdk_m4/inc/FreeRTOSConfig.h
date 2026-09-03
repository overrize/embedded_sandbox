#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

/*
 * FreeRTOSConfig.h for the QEMU `-M mps2-an386` target. Mirrors
 * mdl/tests/hil/at32f435_m2/inc/FreeRTOSConfig.h closely -- the one
 * load-bearing difference is __NVIC_PRIO_BITS: this target's
 * cmsis_device.h defines 3 (CMSDK convention), not AT32F435's 4, which
 * shifts every priority-field constant below by one extra bit.
 */

#define configUSE_PREEMPTION                     1
#define configUSE_PORT_OPTIMISED_TASK_SELECTION  0
#define configUSE_TICKLESS_IDLE                  0
#define configCPU_CLOCK_HZ                       (25000000UL) /* QEMU's Cortex-M4 model doesn't model
                                                                 * real clock timing -- this only affects
                                                                 * SysTick reload math, not actual wall-clock
                                                                 * speed; an approximate value is fine here. */
#define configTICK_RATE_HZ                       (1000)
#define configMAX_PRIORITIES                     (8)
#define configMINIMAL_STACK_SIZE                 (128)
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
#define configTOTAL_HEAP_SIZE                      (16 * 1024)
#define configAPPLICATION_ALLOCATED_HEAP           0

#define configTOTAL_MPU_REGIONS                    8
#define configENFORCE_SYSTEM_CALLS_FROM_KERNEL_ONLY 1
#define configTEX_S_C_B_FLASH                      (0x07UL)
#define configTEX_S_C_B_SRAM                       (0x07UL)
#define configENABLE_MPU                           1
#define configENABLE_FPU                           1
#define configENABLE_TRUSTZONE                     0
#define configRUN_FREERTOS_SECURE_ONLY             0

#define configUSE_IDLE_HOOK                      0
#define configUSE_TICK_HOOK                      0
#define configCHECK_FOR_STACK_OVERFLOW            2
#define configUSE_MALLOC_FAILED_HOOK              1
#define configUSE_DAEMON_TASK_STARTUP_HOOK        0

#define configGENERATE_RUN_TIME_STATS             0
#define configUSE_TRACE_FACILITY                  0
#define configUSE_STATS_FORMATTING_FUNCTIONS      0

#define configUSE_CO_ROUTINES                    0
#define configMAX_CO_ROUTINE_PRIORITIES          (2)

#define configUSE_TIMERS                         1
#define configTIMER_TASK_PRIORITY                (configMAX_PRIORITIES - 2)
#define configTIMER_QUEUE_LENGTH                 10
#define configTIMER_TASK_STACK_DEPTH             (configMINIMAL_STACK_SIZE * 2)

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

/* __NVIC_PRIO_BITS == 8 here (cmsis_device.h -- measured against this
 * QEMU build, see that file's comment), not AT32F435's 4. With the full
 * 8-bit field available, these constants need no left-shift at all
 * (shift = 8-8 = 0) -- unlike AT32's (8-4)=4 shift. Get this wrong and
 * priority-masking (configMAX_SYSCALL_INTERRUPT_PRIORITY) silently masks
 * the wrong set of interrupts. */
#define configKERNEL_INTERRUPT_PRIORITY          (255)
#define configMAX_SYSCALL_INTERRUPT_PRIORITY     (64)
#define configPRIO_BITS                          8

#define configASSERT_DEFINED                     1
void mdl_freertos_assert_failed(const char *file, int line);
#define configASSERT(x)  if ((x) == 0) { mdl_freertos_assert_failed(__FILE__, __LINE__); }

#define vPortSVCHandler       SVC_Handler
#define xPortPendSVHandler    PendSV_Handler
#define xPortSysTickHandler   SysTick_Handler

#endif /* FREERTOS_CONFIG_H */
