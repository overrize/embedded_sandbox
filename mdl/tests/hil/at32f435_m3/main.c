/*
 * M3 acceptance firmware: same load/run path as M2, but the loader task
 * now stays alive forever as the supervisor (mdl/core/supervisor.c) --
 * fault recovery and the software watchdog both go through it. Load a
 * KNOWN-BAD module here (see mdl/tests/modules/fault_*) to exercise
 * this; the "hello" module from M1/M2 exercises the happy path (loads,
 * runs once, self-deletes normally -- supervisor sees state go straight
 * to MDL_SLOT_LOADED and does nothing).
 *
 * NOT WIRED TO A BUILD YET beyond this project's own Makefile -- same
 * caveats as M0/M1/M2 (real AT32F435 BSP required, no hardware
 * execution verified in this sandbox). Which .mdl gets embedded is
 * controlled by the Makefile's MODULE variable.
 */
#include <stdint.h>
#include "FreeRTOS.h"
#include "task.h"
#include "at32f435_437.h"
#include "registry.h"
#include "loader.h"
#include "module_task.h"
#include "host_api.h"
#include "supervisor.h"

extern const uint8_t _binary_module_mdl_start[];
extern const uint8_t _binary_module_mdl_end[];

void mdl_freertos_assert_failed(const char *file, int line)
{
    (void)file;
    (void)line;
    __asm volatile("bkpt #0");
    for (;;) {
    }
}

static void supervisor_task(void *pvParameters)
{
    (void)pvParameters;

    registry_init();
    host_api_init();
    mdl_supervisor_init();

    extern uint8_t __mdl_text_start[], __mdl_text_end[];
    extern uint8_t __mdl_data_start[], __mdl_data_end[];
    extern uint8_t __mdl_heap_stack_start[], __mdl_heap_stack_end[];

    g_mdl_slot.text_lo       = (uintptr_t)__mdl_text_start;
    g_mdl_slot.text_hi       = (uintptr_t)__mdl_text_end;
    g_mdl_slot.data_lo       = (uintptr_t)__mdl_data_start;
    g_mdl_slot.data_hi       = (uintptr_t)__mdl_data_end;
    g_mdl_slot.heap_stack_lo = (uintptr_t)__mdl_heap_stack_start;
    g_mdl_slot.heap_stack_hi = (uintptr_t)__mdl_heap_stack_end;

    size_t image_len = (size_t)(_binary_module_mdl_end - _binary_module_mdl_start);
    mdl_load_status_t st = mdl_load(&g_mdl_slot, _binary_module_mdl_start, image_len,
                                     HOST_API_ABI_VERSION, MDL_ARCH_ARMV7M);

    if (st == MDL_LOAD_OK) {
        host_api_pool_reset(&g_mdl_slot);
        mdl_start_module_task(&g_mdl_slot, &g_host_api);
    }

    /* Never returns -- this task IS the supervisor from here on. A real
     * M4 build instead waits here on a USB-CDC-fed "load this module"
     * queue between fault-recovery wakeups; v1's one-shot load above
     * stands in for that. */
    mdl_supervisor_run();
}

int main(void)
{
    /*
     * MemManage_Handler calls mdl_supervisor_notify_fault_from_isr(),
     * an ISR-safe FreeRTOS call (vTaskNotifyGiveFromISR +
     * portYIELD_FROM_ISR) -- safe only if this fault's priority is at
     * or below (numerically >=) configMAX_SYSCALL_INTERRUPT_PRIORITY.
     * NVIC_SetPriority takes an UNSHIFTED priority (0..15 for this
     * target's 4 priority bits); configMAX_SYSCALL_INTERRUPT_PRIORITY
     * in FreeRTOSConfig.h is pre-shifted for direct SHPR register use,
     * so shift it back down here.
     */
    NVIC_SetPriority(MemoryManagement_IRQn,
                      configMAX_SYSCALL_INTERRUPT_PRIORITY >> (8 - configPRIO_BITS));

    xTaskCreate(supervisor_task, "supervisor", configMINIMAL_STACK_SIZE * 2,
                NULL, MDL_LOADER_TASK_PRIORITY, NULL);

    vTaskStartScheduler();

    for (;;) {
        __asm volatile("bkpt #0");
    }
}
