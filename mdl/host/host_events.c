#include "host_events.h"
#include "host_api.h"
#include "registry.h"
#include "at32f435_437.h"
#include "FreeRTOS.h"
#include "task.h"

/*
 * The queue lives in host memory and is never mapped into the MDL's MPU
 * regions -- an MDL reaches it only through the gated mdl_events_take(),
 * which copies one record out. A queue an MDL could write to directly
 * would let it forge its own events.
 */
static mdl_event_t s_q[MDL_EVT_QUEUE_MAX];
static uint8_t     s_head, s_tail, s_count;
static uint8_t     s_depth = MDL_EVT_QUEUE_MAX;

static uint32_t s_delivered, s_lost, s_coalesced;
static uint16_t s_declared_hz;

/* Rolling 1s window, so the host can say "you declared N/s, I saw M/s"
 * instead of only reporting that events went missing. */
static uint32_t s_window_start, s_window_count, s_peak_hz;

static TaskHandle_t s_module_task;

/* Which EXINT lines we armed, so disarm knows what to undo. */
static uint32_t s_armed_lines;

void mdl_events_reset(uint16_t depth, uint16_t rate_hz, void *module_task_handle)
{
    s_head = s_tail = s_count = 0;
    s_delivered = s_lost = s_coalesced = 0;
    s_window_start = (uint32_t)xTaskGetTickCount();
    s_window_count = 0;
    s_peak_hz = 0;
    s_declared_hz = rate_hz;
    s_depth = (depth == 0u || depth > MDL_EVT_QUEUE_MAX) ? MDL_EVT_QUEUE_MAX
                                                          : (uint8_t)depth;
    s_module_task = (TaskHandle_t)module_task_handle;
}

void mdl_events_get_stats(mdl_events_stats_t *out)
{
    out->delivered   = s_delivered;
    out->lost        = s_lost;
    out->coalesced   = s_coalesced;
    out->declared_hz = s_declared_hz;
    out->peak_hz     = s_peak_hz;
}

/*
 * Push one event, merging into an existing one from the same source when
 * the MDL has not drained fast enough.
 *
 * Merging is right for a STATE-like source: the pin has a level, and only
 * the newest one is true. It would be wrong to merge silently for a
 * COUNT-like source, which is why the merge count rides along in the
 * event -- a handler that cares can recover exactly how many were folded
 * together. Losing that number is how "mitigation" turns into corruption.
 *
 * Runs in interrupt context. No FreeRTOS call here except the ISR-safe
 * notify at the end.
 */
static void post_from_isr(uint8_t source, uint8_t id, uint32_t payload)
{
    uint32_t now = (uint32_t)xTaskGetTickCountFromISR();

    /* Rate accounting, for the declared-vs-actual report. */
    if ((uint32_t)(now - s_window_start) >= 1000u) {
        if (s_window_count > s_peak_hz) {
            s_peak_hz = s_window_count;
        }
        s_window_start = now;
        s_window_count = 0;
    }
    s_window_count++;

    for (uint8_t n = 0, i = s_head; n < s_count; n++, i = (uint8_t)((i + 1u) % MDL_EVT_QUEUE_MAX)) {
        if (s_q[i].source == source && s_q[i].id == id) {
            s_q[i].payload = payload; /* newest value wins */
            if (s_q[i].coalesced < 0xFFFFu) {
                s_q[i].coalesced++;
            }
            s_coalesced++;
            goto wake;
        }
    }

    if (s_count >= s_depth) {
        /* Queue full of OTHER sources: this one cannot even be merged.
         * Real loss, counted and reported -- never silently dropped. */
        s_lost++;
        goto wake;
    }

    s_q[s_tail].source    = source;
    s_q[s_tail].id        = id;
    s_q[s_tail].coalesced = 1u;
    s_q[s_tail].payload   = payload;
    s_q[s_tail].tick_ms   = now;
    s_tail = (uint8_t)((s_tail + 1u) % MDL_EVT_QUEUE_MAX);
    s_count++;

wake:
    if (s_module_task != NULL) {
        BaseType_t woken = pdFALSE;
        vTaskNotifyGiveFromISR(s_module_task, &woken);
        portYIELD_FROM_ISR(woken);
    }
}

bool mdl_events_take(mdl_event_t *out)
{
    bool got = false;
    taskENTER_CRITICAL();
    if (s_count > 0u) {
        *out = s_q[s_head];
        s_head = (uint8_t)((s_head + 1u) % MDL_EVT_QUEUE_MAX);
        s_count--;
        s_delivered++;
        got = true;
    }
    taskEXIT_CRITICAL();
    return got;
}

bool mdl_events_post_console(void)
{
    bool ok = false;
    taskENTER_CRITICAL();
    if (s_count < s_depth) {
        s_q[s_tail].source    = (uint8_t)MDL_EVT_CONSOLE;
        s_q[s_tail].id        = 0;
        s_q[s_tail].coalesced = 1u;
        s_q[s_tail].payload   = 0;
        s_q[s_tail].tick_ms   = (uint32_t)xTaskGetTickCount();
        s_tail = (uint8_t)((s_tail + 1u) % MDL_EVT_QUEUE_MAX);
        s_count++;
        ok = true;
    }
    taskEXIT_CRITICAL();
    if (ok && s_module_task != NULL) {
        xTaskNotifyGive(s_module_task);
    }
    return ok;
}

/* ---- the interrupt half ---------------------------------------------- */

/*
 * Only whitelist pins get here, and only ones the loaded MDL declared with
 * MDL_RES_GPIO_IRQ. host_gpio_exti_info() owns the board knowledge (which
 * EXINT line and IRQ a pin is on); this file owns the queue.
 */
static void gpio_isr(int pin, uint32_t line)
{
    exint_flag_clear(line);
    post_from_isr((uint8_t)MDL_EVT_GPIO, (uint8_t)pin,
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

bool mdl_events_arm_gpio(int pin, uint8_t edge)
{
    uint8_t  port_source, pin_source;
    uint32_t line;
    int      irqn;

    if (edge == (uint8_t)MDL_EDGE_NONE) {
        return true; /* a plain claim, no interrupt wanted */
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
    s_armed_lines |= line;
    return true;
}

void mdl_events_disarm_all(void)
{
    if (s_armed_lines == 0u) {
        return;
    }
    /* Disarm before the task handle goes stale, or a late edge notifies a
     * task that has been deleted. */
    s_module_task = NULL;

    exint_init_type cfg;
    exint_default_para_init(&cfg);
    cfg.line_enable   = FALSE;
    cfg.line_mode     = EXINT_LINE_INTERRUPT;
    cfg.line_select   = s_armed_lines;
    cfg.line_polarity = EXINT_TRIGGER_BOTH_EDGE;
    exint_init(&cfg);
    exint_flag_clear(s_armed_lines);

    if (s_armed_lines & EXINT_LINE_2) {
        nvic_irq_disable(EXINT2_IRQn);
    }
    if (s_armed_lines & EXINT_LINE_3) {
        nvic_irq_disable(EXINT3_IRQn);
    }
    s_armed_lines = 0;
    s_count = s_head = s_tail = 0;
}
