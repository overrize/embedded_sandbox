#include "supervisor.h"
#include "host_api.h" /* g_host_api, host_api_pool_reset() -- supervisor.h
                        * deliberately does NOT pull this in, see its comment */
#include "loader.h"
#include "module_task.h"
#include "host_events.h"
#include "persist.h"
#include "protocol.h"
#include "console.h"
#include "FreeRTOS.h"
#include "task.h"
#include <string.h>

static TaskHandle_t s_supervisor_handle;

/*
 * Reload whatever was saved, at boot [F2].
 *
 * Goes through the ordinary load path, deliberately: what is stored is an
 * IMAGE, so relocation, resource arbitration and task creation all behave
 * exactly as for a fresh push. A separate boot-time loader would be a
 * second code path that only runs at startup and only fails in the field.
 *
 * A saved image that no longer loads -- the ABI moved on, or a pin it
 * claims is now held -- is reported and dropped, and the store is left
 * intact so it can still be inspected. Silently erasing it would hide the
 * reason the device came up empty.
 */
void mdl_supervisor_restore(void)
{
    uint32_t len = 0;
    const void *image = board_persist_image(&len);
    if (image == NULL) {
        return;
    }

    mdl_load_status_t st = mdl_load(&g_mdl_slot, image, len,
                                     HOST_API_ABI_VERSION, MDL_ARCH_ARMV7M);
    if (st != MDL_LOAD_OK) {
        mdl_console_puts("[host] saved MDL rejected at boot: ");
        mdl_console_puts(mdl_load_status_str(st));
        const char *detail = mdl_load_detail();
        if (detail[0] != 0) {
            mdl_console_puts(": ");
            mdl_console_puts(detail);
        }
        mdl_console_puts("\r\n");
        g_mdl_slot.state = MDL_SLOT_EMPTY;
        return;
    }

    host_api_pool_reset(&g_mdl_slot);
    if (!mdl_start_module_task(&g_mdl_slot, &g_host_api)) {
        g_mdl_slot.state = MDL_SLOT_EMPTY;
        return;
    }
    mdl_events_reset(g_mdl_slot.evt_queue_depth, g_mdl_slot.evt_rate_hz,
                      g_mdl_slot.task_handle);
    for (uint8_t i = 0; i < g_mdl_slot.res_count; i++) {
        if (g_mdl_slot.res[i].edge != (uint8_t)MDL_EDGE_NONE) {
            (void)mdl_events_arm_gpio((int)g_mdl_slot.res[i].id,
                                       g_mdl_slot.res[i].edge);
        }
    }
    mdl_console_puts("[host] restored saved MDL: ");
    mdl_console_puts(g_mdl_slot.name);
    mdl_console_puts("\r\n");
}

void mdl_supervisor_init(void)
{
    s_supervisor_handle = xTaskGetCurrentTaskHandle();
    mdl_proto_set_supervisor_handle((void *)s_supervisor_handle);
    mdl_console_set_supervisor_handle((void *)s_supervisor_handle);
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
    g_mdl_slot.parked_until_tick = 0;

    /* Everything the MDL declared about itself goes too.
     *
     * gpio_claimed is the one that bites: host_gpio_yielded_to_module()
     * reads it without consulting slot state, so a stale claim left here
     * kept main.c's indicator task standing down forever and the alive
     * LED never came back after an unload. The other fields happened to
     * be harmless only because `help` and `pins` check the state first --
     * which is not a property to rely on, it is the next bug waiting for
     * someone to add a fourth reader that forgets. */
    /* Restore first, clear second: after the bitmap is zeroed there is
     * no record of which pins to hand back. */
    /* Disarm BEFORE the task handle goes stale: a late edge arriving
     * after the task is deleted would notify a task that is gone. */
    mdl_events_disarm_all();

    uint32_t restored = host_gpio_release_claims(g_mdl_slot.gpio_claimed);
    g_mdl_slot.gpio_claimed = 0;
    g_mdl_slot.res_count    = 0;
    g_mdl_slot.evt_entry    = NULL;
    g_mdl_slot.cmd_pending  = 0u;
    if (restored != 0u) {
        mdl_console_puts("[host] gpio released -> default:");
        for (unsigned i = 0; i < 32u; i++) {
            if ((restored & (1u << i)) != 0u) {
                mdl_console_puts(" ");
                /* single digit is enough; the whitelist is short */
                char d[2];
                d[0] = (char)('0' + (int)i);
                d[1] = 0;
                mdl_console_puts(d);
            }
        }
        mdl_console_puts("\r\n");
    }
    g_mdl_slot.cmd_entry    = NULL;
    g_mdl_slot.cmd_name[0]  = '\0';
    g_mdl_slot.name[0]      = '\0';
    g_mdl_slot.cmd_ret      = 0;
}

void mdl_supervisor_request_unload(void)
{
    if (g_mdl_slot.state == MDL_SLOT_EMPTY) {
        return; /* already unloaded -- not an error, same as MDL_CMD_UNLOAD */
    }
    reclaim_module();
}

static void handle_load(const uint8_t *payload, uint32_t len, bool persist)
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
        /* A resource conflict is the one failure whose generic status
         * string is useless alone -- 'resource conflict' does not say
         * which pin, or who holds it. mdl_load_detail() carries that. */
        const char *msg = mdl_load_status_str(st);
        const char *detail = mdl_load_detail();
        if (detail[0] != 0) {
            char buf[112];
            size_t n = 0;
            for (const char *q = msg; *q != 0 && n < sizeof(buf) - 2u; q++) {
                buf[n++] = *q;
            }
            buf[n++] = ':';
            buf[n++] = ' ';
            for (const char *q = detail; *q != 0 && n < sizeof(buf); q++) {
                buf[n++] = *q;
            }
            mdl_proto_send_response(MDL_RESP_ERROR, buf, (uint32_t)n);
        } else {
            mdl_proto_send_response(MDL_RESP_ERROR, msg, (uint32_t)strlen(msg));
        }
        return;
    }

    host_api_pool_reset(&g_mdl_slot);
    if (!mdl_start_module_task(&g_mdl_slot, &g_host_api)) {
        mdl_proto_send_response(MDL_RESP_ERROR, "xTaskCreateRestricted failed", 29);
        g_mdl_slot.state = MDL_SLOT_EMPTY;
        return;
    }

    /* Arm declared interrupts only now: the ISR notifies the module task,
     * so the task has to exist first. */
    mdl_events_reset(g_mdl_slot.evt_queue_depth, g_mdl_slot.evt_rate_hz,
                      g_mdl_slot.task_handle);
    for (uint8_t i = 0; i < g_mdl_slot.res_count; i++) {
        if (g_mdl_slot.res[i].edge != (uint8_t)MDL_EDGE_NONE) {
            (void)mdl_events_arm_gpio((int)g_mdl_slot.res[i].id,
                                       g_mdl_slot.res[i].edge);
        }
    }

    /* Persist only once the MDL is actually running. Saving an image that
     * then fails to start would hand the board something to reload into
     * the same failure on every boot -- a brick that recreates itself. */
    if (persist && !board_persist_save(payload, len)) {
        mdl_proto_send_response(MDL_RESP_ERROR,
                                 "running, but the flash save failed", 34);
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

/*
 * How long a console command may run before the supervisor takes the
 * module out. This is the watchdog for this path: mdl_supervisor_run()'s
 * own watchdog loop is not executing while we sit here waiting, because
 * we ARE that task. Generous enough for a human-scale command (the demo
 * module blinks an LED for a couple of seconds), short enough that a
 * module stuck in while(1){} does not wedge the console for good.
 */
#define MDL_CMD_TIMEOUT_MS 5000u
#define MDL_CMD_POLL_MS      10u

mdl_cmd_result_t mdl_supervisor_run_module_command(int argc, char **argv, int *out_ret)
{
    /* RUNNING, not LOADED: the task is resident now and the command is
     * queued to it rather than restarting it. */
    if (g_mdl_slot.state != MDL_SLOT_RUNNING || g_mdl_slot.cmd_entry == NULL) {
        return MDL_CMD_NO_MODULE;
    }
    if (argc < 1 || strcmp(argv[0], g_mdl_slot.cmd_name) != 0) {
        return MDL_CMD_NAME_MISMATCH;
    }

    if (!mdl_post_module_command(&g_mdl_slot, argc, (const char *const *)argv)) {
        return MDL_CMD_START_FAILED;
    }

    /* The module task is strictly lower priority than this one, so it
     * only runs while we are blocked in vTaskDelay(). Polling the slot
     * state is therefore both simple and sufficient -- no notification
     * plumbing, and no risk of racing the module's own completion. */
    for (uint32_t waited = 0; waited < MDL_CMD_TIMEOUT_MS; waited += MDL_CMD_POLL_MS) {
        vTaskDelay(pdMS_TO_TICKS(MDL_CMD_POLL_MS));

        if (g_mdl_slot.cmd_pending == 0u) {
            *out_ret = g_mdl_slot.cmd_ret;
            return MDL_CMD_OK;
        }
        if (g_mdl_slot.state == MDL_SLOT_FAULTED) {
            reclaim_module();
            return MDL_CMD_FAULTED;
        }
    }

    reclaim_module();
    return MDL_CMD_TIMEOUT;
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
            /* Signed compare so tick wraparound stays correct: a parked
             * MDL is blocked inside the host and is not a candidate. */
            bool parked = (g_mdl_slot.parked_until_tick == MDL_PARKED_FOREVER) ||
                           ((g_mdl_slot.parked_until_tick != 0u) &&
                            ((int32_t)(now - g_mdl_slot.parked_until_tick) < 0));
            uint32_t idle_ms = (now - g_mdl_slot.last_active_tick); /* configTICK_RATE_HZ==1000 -> ticks==ms */
            if (!parked && idle_ms > MDL_WATCHDOG_TIMEOUT_MS) {
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
            case MDL_CMD_LOAD:         handle_load(payload, len, false); break;
            case MDL_CMD_LOAD_PERSIST: handle_load(payload, len, true);  break;
            case MDL_CMD_UNLOAD: handle_unload();            break;
            case MDL_CMD_STATUS: handle_status();             break;
            }
        }

        /* A typed console line is handled here, in supervisor context,
         * for the same reason a binary frame is: the USB RX task only
         * assembles it. Every path that touches the module slot --
         * fault recovery, watchdog, protocol, console -- therefore runs
         * single-threaded in this one loop. */
        char *line;
        if (mdl_console_take_line(&line)) {
            mdl_console_execute(line);
        }
    }
}
