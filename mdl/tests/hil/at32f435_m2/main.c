/*
 * M2 acceptance firmware: the same "hello" module from M1, now loaded
 * and run as a real unprivileged FreeRTOS-MPU restricted task. Loading
 * happens in a dedicated loader task (never in an ISR, per the spec's
 * hard RTOS constraint), which then hands off to mdl_start_module_task()
 * and exits. If host_gpio_set()/log() etc. reach real hardware/output
 * at all, they did so only through the SVC-gated vtable -- there is no
 * other path from unprivileged module code to a peripheral register.
 *
 * NOT WIRED TO A BUILD YET beyond this project's own Makefile -- same
 * caveats as M0/M1's main.c (real AT32F435 BSP required, no hardware
 * execution verified in this sandbox).
 */
#include <stdint.h>
#include "FreeRTOS.h"
#include "task.h"
#include "registry.h"
#include "loader.h"
#include "module_task.h"
#include "host_api.h"

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

static void loader_task(void *pvParameters)
{
    (void)pvParameters;

    registry_init();
    host_api_init();

    /* v1's arena bounds come straight from the linker script's symbols
     * -- no sandbox_init()/arch_setup_regions() here: that mechanism was
     * M0/M1's pre-RTOS static MPU config, superseded now that
     * xTaskCreateRestricted() (mdl_start_module_task()) programs the
     * module's 3 regions per-task on every context switch instead. */
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

    /* Loader's job for this one-shot smoke test ends here; a real
     * loader task (M4) loops forever, waiting on a USB-CDC-fed queue
     * for the next load/unload request. */
    vTaskDelete(NULL);
}

int main(void)
{
    xTaskCreate(loader_task, "loader", configMINIMAL_STACK_SIZE * 2,
                NULL, MDL_LOADER_TASK_PRIORITY, NULL);

    vTaskStartScheduler();

    /* Only reached if vTaskStartScheduler() itself fails (e.g. out of
     * heap for the idle/timer task) -- should never happen with
     * configTOTAL_HEAP_SIZE sized as it is. */
    for (;;) {
        __asm volatile("bkpt #0");
    }
}
