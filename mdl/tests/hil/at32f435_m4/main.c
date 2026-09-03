/*
 * M4 acceptance firmware: modules arrive over USB CDC at runtime --
 * this is the first milestone where main.c does NOT embed a module via
 * objcopy (M1-M3's build-time test shim). tools/packer.py + a serial
 * write (tools/watch.py automates this) is the only way a module gets
 * into this firmware from here on.
 *
 * NOT WIRED TO A BUILD YET beyond this project's own Makefile -- same
 * caveats as M0-M3 (real AT32F435 BSP + vendor USB driver required, no
 * hardware execution verified in this sandbox -- USB enumeration
 * timing/electrical behavior in particular cannot be verified without
 * real hardware, per the task's own "QEMU 不模拟 ... USB 时序" note).
 */
#include <stdint.h>
#include "FreeRTOS.h"
#include "task.h"
#include "at32f435_437.h"
#include "registry.h"
#include "host_api.h"
#include "supervisor.h"
#include "usb_cdc.h"

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

    /* "USB CDC 和 loader 任务在任何情况下都要活着" -- this task IS both
     * the loader/supervisor AND (via protocol.c's frame handling in
     * mdl_supervisor_run()) the thing that answers USB requests, so
     * keeping it alive is the whole point of everything M3 built. */
    mdl_supervisor_run();
}

static void usb_task(void *pvParameters)
{
    usb_cdc_rx_task(pvParameters); /* never returns */
}

int main(void)
{
    NVIC_SetPriority(MemoryManagement_IRQn,
                      configMAX_SYSCALL_INTERRUPT_PRIORITY >> (8 - configPRIO_BITS));

    usb_cdc_init();

    xTaskCreate(supervisor_task, "supervisor", configMINIMAL_STACK_SIZE * 2,
                NULL, MDL_LOADER_TASK_PRIORITY, NULL);
    xTaskCreate(usb_task, "usb_cdc", configMINIMAL_STACK_SIZE * 2,
                NULL, MDL_USB_TASK_PRIORITY, NULL);

    vTaskStartScheduler();

    for (;;) {
        __asm volatile("bkpt #0");
    }
}
