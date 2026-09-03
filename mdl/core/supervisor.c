#include "supervisor.h"
#include "loader.h"
#include "module_task.h"
#include "protocol.h"
#include "FreeRTOS.h"
#include "task.h"
#include <string.h>

static TaskHandle_t s_supervisor_handle;

void mdl_supervisor_init(void)
{
    s_supervisor_handle = xTaskGetCurrentTaskHandle();
    mdl_proto_set_supervisor_handle((void *)s_supervisor_handle);
}

void mdl_supervisor_wake_from_isr(void)
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

static void handle_load(const uint8_t *payload, uint32_t len)
{
    if (g_mdl_slot.state == MDL_SLOT_RUNNING || g_mdl_slot.state == MDL_SLOT_LOADED) {
        /* v1: one slot -- an explicit unload is required before loading
         * something new, rather than silently replacing a running
         * module out from under itself. */
        mdl_proto_send_response(MDL_RESP_ERROR, "slot busy -- unload first", 25);
        return;
    }

    mdl_load_status_t st = mdl_load(&g_mdl_slot, payload, len,
                                     HOST_API_ABI_VERSION, MDL_ARCH_ARMV7M);
    if (st != MDL_LOAD_OK) {
        const char *msg = mdl_load_status_str(st);
        mdl_proto_send_response(MDL_RESP_ERROR, msg, (uint32_t)strlen(msg));
        return;
    }

    host_api_pool_reset(&g_mdl_slot);
    if (!mdl_start_module_task(&g_mdl_slot, &g_host_api)) {
        mdl_proto_send_response(MDL_RESP_ERROR, "xTaskCreateRestricted failed", 29);
        g_mdl_slot.state = MDL_SLOT_EMPTY;
        return;
    }
    mdl_proto_send_response(MDL_RESP_OK, NULL, 0);
}

static void handle_unload(void)
{
    if (g_mdl_slot.state == MDL_SLOT_EMPTY) {
        mdl_proto_send_response(MDL_RESP_OK, NULL, 0); /* already unloaded -- not an error */
        return;
    }
    reclaim_module();
    mdl_proto_send_response(MDL_RESP_OK, NULL, 0);
}

static void handle_status(void)
{
    mdl_proto_status_t status = { 0 };
    status.state = (uint8_t)g_mdl_slot.state;
    status.fault_pc = g_mdl_last_fault.occurred ? g_mdl_last_fault.pc : 0;
    status.fault_text_offset = g_mdl_last_fault.occurred ? g_mdl_last_fault.text_offset : 0xFFFFFFFFu;
    mdl_proto_send_response(MDL_RESP_STATUS, &status, sizeof(status));
}

void mdl_supervisor_run(void)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(MDL_WATCHDOG_POLL_MS));

        if (g_mdl_slot.state == MDL_SLOT_FAULTED) {
            /* Already classified and marked by fault_arm.c's handler --
             * just reclaim. */
            reclaim_module();
        } else if (g_mdl_slot.state == MDL_SLOT_RUNNING) {
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

        mdl_proto_cmd_t cmd;
        const uint8_t *payload;
        uint32_t len;
        if (mdl_proto_take_frame(&cmd, &payload, &len)) {
            switch (cmd) {
            case MDL_CMD_LOAD:   handle_load(payload, len); break;
            case MDL_CMD_UNLOAD: handle_unload();            break;
            case MDL_CMD_STATUS: handle_status();             break;
            }
        }
    }
}
