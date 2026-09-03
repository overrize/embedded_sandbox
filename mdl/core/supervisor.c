#include "supervisor.h"
#include "FreeRTOS.h"
#include "task.h"

static TaskHandle_t s_supervisor_handle;

void mdl_supervisor_init(void)
{
    s_supervisor_handle = xTaskGetCurrentTaskHandle();
}

void mdl_supervisor_notify_fault_from_isr(void)
{
    BaseType_t woken = pdFALSE;
    vTaskNotifyGiveFromISR(s_supervisor_handle, &woken);
    portYIELD_FROM_ISR(woken);
}

static void reclaim_module(void)
{
    if (g_mdl_slot.task_handle != NULL) {
        vTaskDelete((TaskHandle_t)g_mdl_slot.task_handle);
        g_mdl_slot.task_handle = NULL;
    }
    /* Force-reclaim the alloc() pool regardless of what the module did
     * or didn't free -- "整池回收" per host_api.h's alloc()/free() doc.
     * host_api_pool_reset() is idempotent; safe to call even if the
     * module never allocated anything. */
    host_api_pool_reset(&g_mdl_slot);
    g_mdl_slot.state = MDL_SLOT_EMPTY;
    g_mdl_slot.entry = NULL;
    g_mdl_slot.last_active_tick = 0;
}

void mdl_supervisor_run(void)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(MDL_WATCHDOG_POLL_MS));

        if (g_mdl_slot.state == MDL_SLOT_FAULTED) {
            /* Already classified and marked by fault_arm.c's handler --
             * just reclaim. */
            reclaim_module();
            continue;
        }

        if (g_mdl_slot.state == MDL_SLOT_RUNNING) {
            uint32_t now = (uint32_t)xTaskGetTickCount();
            uint32_t idle_ms = (now - g_mdl_slot.last_active_tick); /* configTICK_RATE_HZ==1000 -> ticks==ms */
            if (idle_ms > MDL_WATCHDOG_TIMEOUT_MS) {
                /* Presumed hung (the while(1){} case: no host call, ever,
                 * for the whole timeout window) -- same treatment as a
                 * real fault, just discovered by timeout instead of by
                 * MemManage. No fault-info to record (nothing trapped),
                 * so g_mdl_last_fault is left as whatever it last was. */
                g_mdl_slot.state = MDL_SLOT_FAULTED;
                reclaim_module();
            }
        }
    }
}
