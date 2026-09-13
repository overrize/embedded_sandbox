#include "host_events.h"
#include "host_api.h"
#include "registry.h"
#include "at32f435_437.h"
#include "FreeRTOS.h"
#include "task.h"

/*
 * One queue per slot [S2/S3].
 *
 * This was a single queue, a single task handle and a single armed-lines
 * mask, all of which read as "the module's" while there was one. Two of
 * them turn into real faults the moment there are two modules:
 *
 *   the task handle   an edge on A's pin would wake whichever module
 *                     registered last
 *   the armed mask    reclaiming A called disarm_all() and took B's
 *                     button away, silently
 *
 * The second is what turned S3 from a follow-on into a prerequisite of
 * multi-slot loading. Nothing about the two-stage dispatch changes: the
 * ISR is still host code that records and wakes, and the module's handler
 * still runs unprivileged in its own task. What changes is WHOSE.
 *
 * The queue lives in host memory and is never mapped into any module's MPU
 * regions -- a module reaches it only through the gated mdl_events_take(),
 * which copies one record out of ITS OWN queue. A queue a module could
 * write to directly would let it forge its own events; a shared queue
 * would let it read its neighbour's.
 */
typedef struct {
    mdl_event_t  q[MDL_EVT_QUEUE_MAX];
    uint8_t      head, tail, count, depth;

    uint32_t     delivered, lost, coalesced;
    uint16_t     declared_hz;

    /* Rolling 1s window, so the host can say "you declared N/s, I saw M/s"
     * rather than only reporting that events went missing. */
    uint32_t     window_start, window_count, peak_hz;

    TaskHandle_t task;
    uint32_t     armed_lines;
} evt_slot_t;

static evt_slot_t s_evt[MDL_MAX_SLOTS];

/*
 * Which slot owns a whitelist pin, or -1.
 *
 * Kept as a flat map rather than looked up by walking module_t's res[]
 * from the ISR: an interrupt handler should not be reading the loader's
 * data structures, and the map is written only at arm/disarm time from
 * task context. 32 matches the bound gpio_claimed already imposes.
 */
static int8_t s_pin_slot[32];

static bool slot_ok(int slot)
{
    return slot >= 0 && slot < MDL_MAX_SLOTS;
}

void mdl_events_reset(int slot, uint16_t depth, uint16_t rate_hz,
                       void *module_task_handle)
{
    if (!slot_ok(slot)) {
        return;
    }
    evt_slot_t *e = &s_evt[slot];
    e->head = e->tail = e->count = 0;
    e->delivered = e->lost = e->coalesced = 0;
    e->window_start = (uint32_t)xTaskGetTickCount();
    e->window_count = 0;
    e->peak_hz = 0;
    e->declared_hz = rate_hz;
    e->depth = (depth == 0u || depth > MDL_EVT_QUEUE_MAX) ? MDL_EVT_QUEUE_MAX
                                                           : (uint8_t)depth;
    e->task = (TaskHandle_t)module_task_handle;
}

void mdl_events_get_stats(int slot, mdl_events_stats_t *out)
{
    if (!slot_ok(slot)) {
        out->delivered = out->lost = out->coalesced = 0;
        out->declared_hz = 0;
        out->peak_hz = 0;
        return;
    }
    const evt_slot_t *e = &s_evt[slot];
    out->delivered   = e->delivered;
    out->lost        = e->lost;
    out->coalesced   = e->coalesced;
    out->declared_hz = e->declared_hz;
    out->peak_hz     = e->peak_hz;
}

/*
 * Push one event into one slot's queue, merging into an existing one from
 * the same source when that slot has not drained fast enough.
 *
 * Merging is right for a STATE-like source: the pin has a level, and only
 * the newest one is true. It would be wrong to merge silently for a
 * COUNT-like source, which is why the merge count rides along in the event
 * -- a handler that cares can recover exactly how many were folded
 * together. Losing that number is how "mitigation" turns into corruption.
 *
 * Runs in interrupt context. No FreeRTOS call here except the ISR-safe
 * notify at the end.
 */
static void post_from_isr(int slot, uint8_t source, uint8_t id, uint32_t payload)
{
    if (!slot_ok(slot)) {
        return;   /* nobody owns this source; dropping is the honest answer */
    }
    evt_slot_t *e = &s_evt[slot];
    uint32_t now = (uint32_t)xTaskGetTickCountFromISR();

    if ((uint32_t)(now - e->window_start) >= 1000u) {
        if (e->window_count > e->peak_hz) {
            e->peak_hz = e->window_count;
        }
        e->window_start = now;
        e->window_count = 0;
    }
    e->window_count++;

    for (uint8_t n = 0, i = e->head; n < e->count;
         n++, i = (uint8_t)((i + 1u) % MDL_EVT_QUEUE_MAX)) {
        if (e->q[i].source == source && e->q[i].id == id) {
            e->q[i].payload = payload; /* newest value wins */
            if (e->q[i].coalesced < 0xFFFFu) {
                e->q[i].coalesced++;
            }
            e->coalesced++;
            goto wake;
        }
    }

    if (e->count >= e->depth) {
        /* Queue full of OTHER sources: this one cannot even be merged.
         * Real loss, counted and reported -- never silently dropped. */
        e->lost++;
        goto wake;
    }

    e->q[e->tail].source    = source;
    e->q[e->tail].id        = id;
    e->q[e->tail].coalesced = 1u;
    e->q[e->tail].payload   = payload;
    e->q[e->tail].tick_ms   = now;
    /* Stamped in the ISR, not when the task picks it up -- the whole point
     * is to measure the gap between those two moments. */
    e->q[e->tail].cycles    = host_cycles_now();
    e->tail = (uint8_t)((e->tail + 1u) % MDL_EVT_QUEUE_MAX);
    e->count++;

wake:
    if (e->task != NULL) {
        BaseType_t woken = pdFALSE;
        vTaskNotifyGiveFromISR(e->task, &woken);
        portYIELD_FROM_ISR(woken);
    }
}

void mdl_events_post_uart(uint8_t instance, uint16_t waiting)
{
    /* A UART belongs to whichever slot declared it. Walking the slots from
     * an ISR is acceptable here where it would not be for GPIO: a byte
     * arrives at most every ~87us at 115200, while a pin can bounce far
     * faster, and there is no pin-style index to precompute against. */
    for (int i = 0; i < MDL_MAX_SLOTS; i++) {
        const module_t *m = &g_mdl_slots[i];
        if (m->state == MDL_SLOT_EMPTY) {
            continue;
        }
        for (uint8_t r = 0; r < m->res_count; r++) {
            if (m->res[r].kind == (uint8_t)MDL_RES_KIND_UART &&
                m->res[r].id == instance) {
                /* payload is how many bytes are waiting, so a handler can
                 * size one read instead of looping. It is a snapshot: more
                 * may arrive between the ISR and the handler, which is why
                 * read() reports its own count and this is a hint. */
                post_from_isr(i, (uint8_t)MDL_EVT_UART, instance,
                               (uint32_t)waiting);
                return;
            }
        }
    }
}

bool mdl_events_take(mdl_event_t *out)
{
    /* The CALLER's queue. Identity comes from the task, exactly as it does
     * for every other gated call -- a slot index passed in by the module
     * would be a slot index the module could lie about. */
    int slot = mdl_slot_index(mdl_caller_slot());
    if (!slot_ok(slot)) {
        return false;
    }
    evt_slot_t *e = &s_evt[slot];

    bool got = false;
    taskENTER_CRITICAL();
    if (e->count > 0u) {
        *out = e->q[e->head];
        e->head = (uint8_t)((e->head + 1u) % MDL_EVT_QUEUE_MAX);
        e->count--;
        e->delivered++;
        got = true;
    }
    taskEXIT_CRITICAL();
    return got;
}

bool mdl_events_post_console(int slot)
{
    if (!slot_ok(slot)) {
        return false;
    }
    evt_slot_t *e = &s_evt[slot];

    bool ok = false;
    taskENTER_CRITICAL();
    if (e->count < e->depth) {
        e->q[e->tail].source    = (uint8_t)MDL_EVT_CONSOLE;
        e->q[e->tail].id        = 0;
        e->q[e->tail].coalesced = 1u;
        e->q[e->tail].payload   = 0;
        e->q[e->tail].tick_ms   = (uint32_t)xTaskGetTickCount();
        e->q[e->tail].cycles    = host_cycles_now();
        e->tail = (uint8_t)((e->tail + 1u) % MDL_EVT_QUEUE_MAX);
        e->count++;
        ok = true;
    }
    taskEXIT_CRITICAL();
    if (ok && e->task != NULL) {
        xTaskNotifyGive(e->task);
    }
    return ok;
}

/* ---- the interrupt half ---------------------------------------------- */

/*
 * Only whitelist pins get here, and only ones a loaded module declared
 * with MDL_RES_GPIO_IRQ. host_gpio_exti_info() owns the board knowledge
 * (which EXINT line and IRQ a pin is on); this file owns the queues.
 */
static void gpio_isr(int pin, uint32_t line)
{
    exint_flag_clear(line);
    int slot = (pin >= 0 && pin < 32) ? s_pin_slot[pin] : -1;
    post_from_isr(slot, (uint8_t)MDL_EVT_GPIO, (uint8_t)pin,
                   (uint32_t)host_gpio_direct_get(pin));
}

/* PA3 = whitelist pin 2 = EXINT line 3; PE2 = whitelist pin 3 = line 2.
 * Both have their own vector on this part -- lines 5..15 share one, which
 * is why extending past these two needs a demultiplexing handler rather
 * than another copy of this. */
void EXINT3_IRQHandler(void);
void EXINT3_IRQHandler(void)
{
    gpio_isr(2, EXINT_LINE_3);
}

void EXINT2_IRQHandler(void);
void EXINT2_IRQHandler(void)
{
    gpio_isr(3, EXINT_LINE_2);
}

bool mdl_events_arm_gpio(int slot, int pin, uint8_t edge)
{
    uint8_t  port_source, pin_source;
    uint32_t line;
    int      irqn;

    if (edge == (uint8_t)MDL_EDGE_NONE) {
        return true; /* a plain claim, no interrupt wanted */
    }
    if (!slot_ok(slot) || pin < 0 || pin >= 32) {
        return false;
    }
    if (!host_gpio_exti_info(pin, &port_source, &pin_source, &line, &irqn)) {
        return false; /* not a pin that can raise one */
    }

    crm_periph_clock_enable(CRM_SCFG_PERIPH_CLOCK, TRUE);
    scfg_exint_line_config((scfg_port_source_type)port_source,
                            (scfg_pins_source_type)pin_source);

    exint_init_type cfg;
    exint_default_para_init(&cfg);
    cfg.line_enable   = TRUE;
    cfg.line_mode     = EXINT_LINE_INTERRUPT;
    cfg.line_select   = line;
    cfg.line_polarity = (edge == (uint8_t)MDL_EDGE_RISING)  ? EXINT_TRIGGER_RISING_EDGE
                       : (edge == (uint8_t)MDL_EDGE_FALLING) ? EXINT_TRIGGER_FALLING_EDGE
                                                             : EXINT_TRIGGER_BOTH_EDGE;
    exint_init(&cfg);
    exint_flag_clear(line);

    /* Priority 6: numerically above configMAX_SYSCALL_INTERRUPT_PRIORITY
     * (5), so this ISR may use the ISR-safe FreeRTOS API. A latency-
     * critical host timer would go BELOW 5 instead and be immune to the
     * kernel entirely -- at the price of not being allowed to call it. */
    nvic_irq_enable((IRQn_Type)irqn, 6, 0);

    s_pin_slot[pin] = (int8_t)slot;
    s_evt[slot].armed_lines |= line;
    return true;
}

void mdl_events_disarm_slot(int slot)
{
    if (!slot_ok(slot)) {
        return;
    }
    evt_slot_t *e = &s_evt[slot];

    /* Stop the wake before the handle goes stale: a late edge arriving
     * after the task is deleted would notify a task that is gone. */
    e->task = NULL;

    if (e->armed_lines == 0u) {
        e->count = e->head = e->tail = 0;
        return;
    }

    exint_init_type cfg;
    exint_default_para_init(&cfg);
    cfg.line_enable   = FALSE;
    cfg.line_mode     = EXINT_LINE_INTERRUPT;
    cfg.line_select   = e->armed_lines;
    cfg.line_polarity = EXINT_TRIGGER_BOTH_EDGE;
    exint_init(&cfg);
    exint_flag_clear(e->armed_lines);

    /* Only this slot's vectors. Disabling one another slot still uses
     * would be exactly the silent failure this rework exists to remove. */
    if (e->armed_lines & EXINT_LINE_2) {
        nvic_irq_disable(EXINT2_IRQn);
    }
    if (e->armed_lines & EXINT_LINE_3) {
        nvic_irq_disable(EXINT3_IRQn);
    }
    e->armed_lines = 0;
    e->count = e->head = e->tail = 0;

    for (int i = 0; i < 32; i++) {
        if (s_pin_slot[i] == (int8_t)slot) {
            s_pin_slot[i] = -1;
        }
    }
}

void mdl_events_init(void)
{
    for (int i = 0; i < 32; i++) {
        s_pin_slot[i] = -1;
    }
    for (int i = 0; i < MDL_MAX_SLOTS; i++) {
        s_evt[i].task = NULL;
        s_evt[i].armed_lines = 0;
        s_evt[i].count = s_evt[i].head = s_evt[i].tail = 0;
        s_evt[i].depth = MDL_EVT_QUEUE_MAX;
    }
}
