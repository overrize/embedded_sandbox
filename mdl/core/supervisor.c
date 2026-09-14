#include "supervisor.h"
#include "arena.h"
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

/* Defined below, next to handle_load(); declared here because
 * mdl_supervisor_restore() is above it and must use the same startup
 * path -- a restored MDL coming up differently from a pushed one is a
 * difference that would only ever show at a customer site. */
static bool start_loaded_module(module_t *slot);

/* Slot selection, defined below. Declared here because boot restore needs
 * them and runs before them in the file. */
static module_t *find_slot_by_name(const char *name);
static module_t *find_free_slot(void);
static module_t *find_slot_by_command(const char *cmd);


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
    /*
     * Every saved image, not the saved image [S5].
     *
     * A board that comes back running one of the three modules it had is
     * not a board that survived a power cut -- it is a board that lost two
     * thirds of itself and did not say so.
     *
     * Each goes through the ordinary load path, so a restored module is
     * relocated and arbitrated exactly like a pushed one. A stored image
     * that has become unloadable (a firmware whose ABI moved on, a pin now
     * taken by an earlier entry) is reported and skipped rather than
     * aborting the rest: losing one module should not cost the others.
     */
    uint32_t n = board_persist_count();
    for (uint32_t k = 0; k < n; k++) {
        uint32_t len = 0;
        const void *image = board_persist_entry(k, &len);
        if (image == NULL) {
            continue;
        }

        module_t *slot = find_free_slot();
        if (slot == NULL) {
            mdl_console_puts("[host] more saved MDLs than slots; stopping\r\n");
            return;
        }

        mdl_load_status_t st = mdl_load(slot, image, len,
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
            slot->state = MDL_SLOT_EMPTY;
            continue;
        }
        if (!start_loaded_module(slot)) {
            slot->state = MDL_SLOT_EMPTY;
            continue;
        }
        mdl_console_puts("[host] restored: ");
        mdl_console_puts(slot->name);
        mdl_console_puts("\r\n");
    }
}

void mdl_supervisor_init(void)
{
    /* Before anything can arm a pin: the map starts as zeros, which would
     * read as "slot 0 owns every pin" rather than "nobody does". */
    mdl_events_init();
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

static void reclaim_slot(module_t *slot)
{
    if (slot->task_handle != NULL) {
        vTaskDelete((TaskHandle_t)slot->task_handle);
        slot->task_handle = NULL;
    }
    /* Force-reclaim the alloc() pool regardless of what the module did
     * or didn't free -- "整池回收" per host_api.h's alloc()/free() doc.
     * host_api_pool_reset() is idempotent; safe to call even if the
     * module never allocated anything. */
    host_api_pool_reset(slot);

    /* Hand the arena blocks back [S1]. After the task is gone and before
     * the bounds are cleared: releasing while the module could still run
     * would put its own memory back in the pool underneath it. */
    mdl_arena_release(slot);
    slot->state = MDL_SLOT_EMPTY;
    slot->entry = NULL;
    slot->last_active_tick = 0;
    slot->parked_until_tick = 0;

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
    /* Only this slot's lines [S2/S3]. disarm_all() here would take a
     * neighbouring module's button away as a side effect of unloading
     * this one -- silently, and long after the cause. */
    mdl_events_disarm_slot(mdl_slot_index(slot));

    /* Release peripherals before the claims are cleared, same ordering
     * reason as the GPIO release below. */
    for (uint8_t i = 0; i < slot->res_count; i++) {
        if (slot->res[i].kind == (uint8_t)MDL_RES_KIND_I2C) {
            host_i2c_release((int)slot->res[i].id);
        }
        if (slot->res[i].kind == (uint8_t)MDL_RES_KIND_UART) {
            host_uart_release((int)slot->res[i].id);
        }
        if (slot->res[i].kind == (uint8_t)MDL_RES_KIND_ADC) {
            host_adc_release((int)slot->res[i].id);
        }
        if (slot->res[i].kind == (uint8_t)MDL_RES_KIND_SPI) {
            host_spi_release((int)slot->res[i].id);
        }
    }

    uint32_t restored = host_gpio_release_claims(slot->gpio_claimed);
    slot->gpio_claimed = 0;
    slot->res_count    = 0;
    slot->evt_entry    = NULL;
    slot->cmd_pending  = 0u;
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
    slot->cmd_entry    = NULL;
    slot->cmd_name[0]  = '\0';
    slot->name[0]      = '\0';
    slot->cmd_ret      = 0;
}

void mdl_supervisor_request_unload(void)
{
    /* Every slot [S2]: this entry point names none, and with more than one
     * resident "unload" without a subject can only mean all of them.
     * Already-empty is not an error, same as MDL_CMD_UNLOAD. */
    for (int i = 0; i < MDL_MAX_SLOTS; i++) {
        if (g_mdl_slots[i].state != MDL_SLOT_EMPTY) {
            reclaim_slot(&g_mdl_slots[i]);
        }
    }
}

/*
 * Send a load failure, with the detail when there is one. 'resource
 * conflict' alone tells the person pushing the MDL nothing they can act
 * on; mdl_load_detail() names the pin and its holder.
 */
/* ---- choosing a slot [S2] ------------------------------------------ */

static module_t *find_slot_by_name(const char *name)
{
    if (name == NULL || name[0] == 0) {
        return NULL;
    }
    for (int i = 0; i < MDL_MAX_SLOTS; i++) {
        if (g_mdl_slots[i].state != MDL_SLOT_EMPTY &&
            strncmp(g_mdl_slots[i].name, name, MDL_NAME_MAX) == 0) {
            return &g_mdl_slots[i];
        }
    }
    return NULL;
}

static module_t *find_free_slot(void)
{
    for (int i = 0; i < MDL_MAX_SLOTS; i++) {
        if (g_mdl_slots[i].state == MDL_SLOT_EMPTY) {
            return &g_mdl_slots[i];
        }
    }
    return NULL;
}

/* The console types a command name; this finds whose it is. */
static module_t *find_slot_by_command(const char *cmd)
{
    for (int i = 0; i < MDL_MAX_SLOTS; i++) {
        module_t *m = &g_mdl_slots[i];
        if (m->state == MDL_SLOT_RUNNING && m->cmd_entry != NULL &&
            m->cmd_name[0] != 0 && strcmp(cmd, m->cmd_name) == 0) {
            return m;
        }
    }
    return NULL;
}

static void send_load_error(mdl_load_status_t st)
{
    const char *msg = mdl_load_status_str(st);
    const char *detail = mdl_load_detail();
    if (detail[0] == 0) {
        mdl_proto_send_response(MDL_RESP_ERROR, msg, (uint32_t)strlen(msg));
        return;
    }
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
}

/*
 * Bring a freshly-loaded MDL to life: pool, task, events, interrupts.
 * Shared by handle_load() and mdl_supervisor_restore() so a restored MDL
 * and a pushed one cannot come up differently.
 */
static bool start_loaded_module(module_t *slot)
{
    host_api_pool_reset(slot);
    if (!mdl_start_module_task(slot, &g_host_api)) {
        return false;
    }
    /* Arm interrupts only now: the ISR notifies the module task, so the
     * task has to exist first. */
    mdl_events_reset(mdl_slot_index(slot), slot->evt_queue_depth,
                      slot->evt_rate_hz, slot->task_handle);
    for (uint8_t i = 0; i < slot->res_count; i++) {
        if (slot->res[i].kind == (uint8_t)MDL_RES_KIND_GPIO &&
            slot->res[i].edge != (uint8_t)MDL_EDGE_NONE) {
            (void)mdl_events_arm_gpio(mdl_slot_index(slot),
                                       (int)slot->res[i].id,
                                       slot->res[i].edge);
        }
        /* A declared bus is brought up here, not on first use: an MDL
         * that declared it should find it working, and a bus that cannot
         * be initialised (wrong APB1 clock) should say so at load rather
         * than surface as a transfer failure later. */
        if (slot->res[i].kind == (uint8_t)MDL_RES_KIND_I2C) {
            if (!host_i2c_claim((int)slot->res[i].id)) {
                mdl_console_puts("[host] i2c bus could not be initialised\r\n");
            }
        }
        if (slot->res[i].kind == (uint8_t)MDL_RES_KIND_UART) {
            if (!host_uart_claim((int)slot->res[i].id)) {
                mdl_console_puts("[host] uart could not be initialised\r\n");
            }
        }
        if (slot->res[i].kind == (uint8_t)MDL_RES_KIND_ADC) {
            if (!host_adc_claim((int)slot->res[i].id)) {
                mdl_console_puts("[host] adc channel could not be claimed\r\n");
            }
        }
        if (slot->res[i].kind == (uint8_t)MDL_RES_KIND_SPI) {
            if (!host_spi_claim((int)slot->res[i].id)) {
                mdl_console_puts("[host] spi could not be initialised\r\n");
            }
        }
    }
    return true;
}
/*
 * Load, replacing whatever is running [F3].
 *
 * The old behaviour was to refuse when the slot was busy, which pushed
 * the problem to the caller: tools did UNLOAD then LOAD, and between
 * those two frames the device had no MDL at all -- pins back to default,
 * interrupts disarmed. If the new image then turned out to be bad, the
 * old feature was already gone and the board simply sat there empty.
 *
 * ONE ARENA MEANS THE WINDOW CANNOT BE ZERO. The new image is copied over
 * the old one's memory, so the old MDL must be destroyed first. What is
 * removable is every REASON to enter that window: the image is fully
 * validated -- magic, CRC, ABI, arch, sizes, relocations, resource
 * conflicts -- while the old MDL is still running. A rejected replacement
 * therefore costs nothing at all.
 *
 * What remains inside the window is a memcpy and a task creation. If the
 * task fails to start there, the old MDL is already gone; the flash store
 * is the recovery path, and the reply says so rather than leaving someone
 * to work out why the device went quiet.
 */
/*
 * Peek at the name inside an image without loading it.
 *
 * Needed before the slot is chosen, because the slot depends on the name:
 * a push of the same module replaces it, a push of a different one takes
 * a new slot. Reads only the header, which mdl_load_validate() is about to
 * check anyway -- a malformed image is rejected a moment later either way.
 */
static const char *image_name(const uint8_t *payload, uint32_t len)
{
    if (len < sizeof(mdl_header_t)) {
        return "";
    }
    return ((const mdl_header_t *)payload)->name;
}

static void handle_load(const uint8_t *payload, uint32_t len, bool persist)
{
    /*
     * WHICH SLOT [S2].
     *
     * Same name replaces; a different name takes a free slot. That keeps
     * hot replace meaning what it meant -- pushing a newer build of a
     * module swaps it -- while pushing a DIFFERENT module now adds it
     * instead of evicting whatever happened to be resident.
     *
     * THIS IS A VISIBLE BEHAVIOUR CHANGE. Two modules that both want one
     * pin used to take turns by accident, because the second push
     * destroyed the first. They coexist now until one asks for a wire the
     * other holds, and then the load is refused by name. The refusal is
     * the point of multi-slot rather than a regression -- but a push
     * sequence that used to work may now need an explicit unload.
     */
    module_t *slot = find_slot_by_name(image_name(payload, len));
    bool replacing = (slot != NULL);
    if (slot == NULL) {
        slot = find_free_slot();
    }
    if (slot == NULL) {
        mdl_proto_send_response(MDL_RESP_ERROR,
            "all slots in use; unload one first", 33);
        return;
    }

    /*
     * One command name, one owner [S4].
     *
     * find_slot_by_command() returns the first match, so two modules
     * offering `run` would make typing it a coin toss -- and a silent one,
     * since both loads succeeded. Refusing the second load turns an
     * ambiguity that shows up later as "the wrong module ran" into a
     * refusal that names the module already using it.
     *
     * Checked here rather than in the loader because a command name is not
     * a resource in the res[] sense -- nothing physical collides. What
     * collides is the console's ability to say who was meant.
     */
    if (len >= sizeof(mdl_header_t)) {
        const mdl_header_t *h = (const mdl_header_t *)payload;
        if (h->cmd_name[0] != 0) {
            module_t *other = find_slot_by_command(h->cmd_name);
            if (other != NULL && other != slot) {
                mdl_console_puts("[host] refusing: command `");
                mdl_console_puts(h->cmd_name);
                mdl_console_puts("` is already ");
                mdl_console_puts(other->name[0] ? other->name : "another module");
                mdl_console_puts("'s\r\n");
                mdl_proto_send_response(MDL_RESP_ERROR,
                    "that console command name is already taken", 41);
                return;
            }
        }
    }

    mdl_load_status_t vst = mdl_load_validate(slot, payload, len,
                                                HOST_API_ABI_VERSION,
                                                MDL_ARCH_ARMV7M);
    if (vst != MDL_LOAD_OK) {
        /* Nothing has been touched: whatever was running still is. */
        send_load_error(vst);
        return;
    }

    if (replacing) {
        reclaim_slot(slot);
    }

    mdl_load_status_t st = mdl_load(slot, payload, len,
                                     HOST_API_ABI_VERSION, MDL_ARCH_ARMV7M);
    if (st != MDL_LOAD_OK) {
        /* Validation passed and the copy still failed -- so this is not a
         * bad image but something wrong on the device side. Say that
         * plainly instead of reporting it as a rejected module. */
        slot->state = MDL_SLOT_EMPTY;
        send_load_error(st);
        return;
    }

    if (!start_loaded_module(slot)) {
        slot->state = MDL_SLOT_EMPTY;
        mdl_proto_send_response(MDL_RESP_ERROR,
            replacing ? "task creation failed; the previous MDL is gone, "
                         "power-cycle to restore the saved one"
                      : "xTaskCreateRestricted failed",
            replacing ? 76u : 28u);
        return;
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
    /* Every slot. The wire protocol's UNLOAD carries no argument, and when
     * there was one module it meant "clear it" -- so clearing all of them
     * is the reading that stays true. Unloading ONE is a console command,
     * where a name can be given. */
    int freed = 0;
    for (int i = 0; i < MDL_MAX_SLOTS; i++) {
        if (g_mdl_slots[i].state != MDL_SLOT_EMPTY) {
            reclaim_slot(&g_mdl_slots[i]);
            freed++;
        }
    }
    (void)freed;   /* already-empty is not an error */
    mdl_proto_send_response(MDL_RESP_OK, NULL, 0);
}

static void handle_status(void)
{
    mdl_proto_status_t status = { 0 };
    /* Slot zero, deliberately. The wire protocol's STATUS frame carries
     * ONE state field, and widening it would break every tool that
     * speaks it. The `slots` console command is the multi-slot view. */
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
    if (argc < 1) {
        return MDL_CMD_NAME_MISMATCH;
    }
    /* Whose command is this? [S2] With one module the answer was implicit.
     * Two modules offering the same name would be ambiguous, which is why
     * a duplicate command name is refused at load. */
    module_t *slot = find_slot_by_command(argv[0]);
    if (slot == NULL) {
        /* Distinguish "nothing is loaded" from "that is not its name" --
         * they send the user to different places. */
        for (int i = 0; i < MDL_MAX_SLOTS; i++) {
            if (g_mdl_slots[i].state == MDL_SLOT_RUNNING &&
                g_mdl_slots[i].cmd_entry != NULL) {
                return MDL_CMD_NAME_MISMATCH;
            }
        }
        return MDL_CMD_NO_MODULE;
    }

    if (!mdl_post_module_command(slot, argc, (const char *const *)argv)) {
        return MDL_CMD_START_FAILED;
    }

    /* The module task is strictly lower priority than this one, so it
     * only runs while we are blocked in vTaskDelay(). Polling the slot
     * state is therefore both simple and sufficient -- no notification
     * plumbing, and no risk of racing the module's own completion. */
    for (uint32_t waited = 0; waited < MDL_CMD_TIMEOUT_MS; waited += MDL_CMD_POLL_MS) {
        vTaskDelay(pdMS_TO_TICKS(MDL_CMD_POLL_MS));

        if (slot->cmd_pending == 0u) {
            *out_ret = slot->cmd_ret;
            return MDL_CMD_OK;
        }
        if (slot->state == MDL_SLOT_FAULTED) {
            reclaim_slot(slot);
            return MDL_CMD_FAULTED;
        }
    }

    /* Only the offending slot. Taking every module out because one
     * wedged would make a neighbour's failure indistinguishable from
     * its own. */
    reclaim_slot(slot);
    return MDL_CMD_TIMEOUT;
}

void mdl_supervisor_run(void)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(MDL_WATCHDOG_POLL_MS));

        /* Every slot, independently [S2]. A module that faults or wedges
         * takes only itself out -- which is the entire point of giving
         * each one its own MPU regions and its own task. Reclaiming the
         * whole table would make a neighbour's bug look like yours. */
        for (int i = 0; i < MDL_MAX_SLOTS; i++) {
            module_t *m = &g_mdl_slots[i];

            if (m->state == MDL_SLOT_FAULTED) {
                reclaim_slot(m);
                continue;
            }
            if (m->state != MDL_SLOT_RUNNING) {
                continue;
            }

            uint32_t now = (uint32_t)xTaskGetTickCount();
            /* Signed compare so tick wraparound stays correct: a parked
             * MDL is blocked inside the host and is not a candidate. */
            bool parked = (m->parked_until_tick == MDL_PARKED_FOREVER) ||
                           ((m->parked_until_tick != 0u) &&
                            ((int32_t)(now - m->parked_until_tick) < 0));
            /* configTICK_RATE_HZ == 1000, so ticks are milliseconds. */
            uint32_t idle_ms = (now - m->last_active_tick);

            if (!parked && idle_ms > MDL_WATCHDOG_TIMEOUT_MS) {
                /* Presumed hung (the while(1){} case: no host call at all
                 * for the whole window) -- same treatment as a real fault,
                 * just discovered by timeout rather than by MemManage. No
                 * fault info to record, so g_mdl_last_fault is left as it
                 * was. */
                m->state = MDL_SLOT_FAULTED;
                reclaim_slot(m);
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
