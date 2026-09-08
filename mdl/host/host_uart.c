/*
 * UART for MDLs. Shaped differently from I2C, because a UART is not a bus
 * you take turns on -- it is a stream that arrives whether or not anyone
 * is listening.
 *
 * That single fact decides everything here:
 *
 *   - RECEIVE IS INTERRUPT-DRIVEN INTO A RING. If reception only happened
 *     while an MDL was inside read(), every byte that arrived between
 *     calls would be lost, and the MDL would have no way to know. The host
 *     always listens; read() takes what accumulated.
 *
 *   - READ DOES NOT BLOCK. A "read n bytes" that waits is a promise about
 *     something the wire has not agreed to. A reader that must guess how
 *     much is coming is how serial protocols deadlock. It returns a count,
 *     possibly zero, and the MDL decides what to do about that.
 *
 *   - OVERFLOW DROPS THE NEWEST AND SAYS SO. When the ring is full the
 *     arriving byte is discarded rather than overwriting the oldest,
 *     because a truncated stream with a valid prefix can still be parsed
 *     while one with a hole in the middle usually cannot. The count is
 *     reported through `status` -- silently losing bytes from a protocol
 *     stream is the kind of fault that gets diagnosed as "the sensor is
 *     flaky" for a week.
 *
 * Transmit stays synchronous and bounded, like I2C: it runs in the MDL's
 * own lowest-priority task, so blocking there blocks only that MDL.
 */
#include "host_api.h"
#include "host_events.h"
#include "registry.h"
#include "mdl_format.h"
#include "at32f435_437.h"
#include "console.h"
#include "FreeRTOS.h"
#include "task.h"
#include <string.h>

#define UART_RX_RING   256u
#define UART_TX_TIMEOUT_LOOPS 2000000u   /* ~ tens of ms at 288MHz */
#define UART_MAX_WRITE 256u
#define UART_DEFAULT_BAUD 115200u

typedef struct {
    uint8_t     instance;
    usart_type *periph;
    crm_periph_clock_type clock;
    IRQn_Type   irqn;

    gpio_type  *port;
    uint16_t    tx_pin, rx_pin;
    gpio_pins_source_type tx_source, rx_source;
    uint8_t     mux;
    crm_periph_clock_type gpio_clock;

    bool     open;
    uint32_t baud;

    /* Written by the ISR, read by the module task. head/tail are single
     * producer / single consumer, so no lock is needed as long as each
     * side only advances its own index -- which is the one rule this
     * arrangement depends on. */
    volatile uint8_t  ring[UART_RX_RING];
    volatile uint16_t head, tail;
    volatile uint32_t dropped;
} uart_bus_t;

static uart_bus_t g_uarts[] = {
    { 2, USART2, CRM_USART2_PERIPH_CLOCK, USART2_IRQn,
      GPIOA, GPIO_PINS_2, GPIO_PINS_3, GPIO_PINS_SOURCE2, GPIO_PINS_SOURCE3,
      GPIO_MUX_7, CRM_GPIOA_PERIPH_CLOCK,
      false, UART_DEFAULT_BAUD, { 0 }, 0, 0, 0 },
};
#define UART_COUNT (sizeof(g_uarts) / sizeof(g_uarts[0]))

static uart_bus_t *uart_lookup(int instance)
{
    for (unsigned i = 0; i < UART_COUNT; i++) {
        if (g_uarts[i].instance == (uint8_t)instance) {
            return &g_uarts[i];
        }
    }
    return NULL;
}

static uint16_t ring_used(const uart_bus_t *u)
{
    return (uint16_t)((u->head - u->tail) % UART_RX_RING);
}

/* ---- the interrupt half ---------------------------------------------- */

static void uart_isr(uart_bus_t *u)
{
    if (usart_flag_get(u->periph, USART_RDBF_FLAG) == SET) {
        uint8_t b = (uint8_t)usart_data_receive(u->periph);
        uint16_t next = (uint16_t)((u->head + 1u) % UART_RX_RING);
        if (next == u->tail) {
            u->dropped++;      /* full: keep the prefix, count the loss */
        } else {
            u->ring[u->head] = b;
            u->head = next;
            mdl_events_post_uart(u->instance, ring_used(u));
        }
    }

    /* Overrun/framing/noise: clear by reading, or the peripheral stops
     * delivering. A framing error is exactly what a UART pin being used as
     * a button produces, so this is not a rare path on this board. */
    if (usart_flag_get(u->periph, USART_ROERR_FLAG) == SET ||
        usart_flag_get(u->periph, USART_FERR_FLAG) == SET ||
        usart_flag_get(u->periph, USART_NERR_FLAG) == SET) {
        (void)usart_data_receive(u->periph);
        u->dropped++;
    }
}

void USART2_IRQHandler(void);
void USART2_IRQHandler(void)
{
    uart_isr(&g_uarts[0]);
}

/* ---- bring-up / tear-down -------------------------------------------- */

static void uart_apply(uart_bus_t *u)
{
    usart_init(u->periph, u->baud, USART_DATA_8BITS, USART_STOP_1_BIT);
    usart_transmitter_enable(u->periph, TRUE);
    usart_receiver_enable(u->periph, TRUE);
    usart_interrupt_enable(u->periph, USART_RDBF_INT, TRUE);
    usart_enable(u->periph, TRUE);
}

bool host_uart_claim(int instance)
{
    uart_bus_t *u = uart_lookup(instance);
    if (u == NULL) {
        return false;
    }
    if (u->open) {
        return true;
    }

    crm_periph_clock_enable(u->gpio_clock, TRUE);
    crm_periph_clock_enable(u->clock, TRUE);

    gpio_init_type gpio_init_struct;
    gpio_default_para_init(&gpio_init_struct);
    gpio_init_struct.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
    gpio_init_struct.gpio_out_type       = GPIO_OUTPUT_PUSH_PULL;
    gpio_init_struct.gpio_mode           = GPIO_MODE_MUX;
    gpio_init_struct.gpio_pull           = GPIO_PULL_NONE;
    gpio_init_struct.gpio_pins           = u->tx_pin;
    gpio_init(u->port, &gpio_init_struct);

    /* RX pulled up: an unconnected receive line otherwise floats and
     * generates a stream of framing errors from noise. */
    gpio_init_struct.gpio_pull = GPIO_PULL_UP;
    gpio_init_struct.gpio_pins = u->rx_pin;
    gpio_init(u->port, &gpio_init_struct);

    gpio_pin_mux_config(u->port, u->tx_source, u->mux);
    gpio_pin_mux_config(u->port, u->rx_source, u->mux);

    u->head = u->tail = 0;
    u->dropped = 0;
    uart_apply(u);

    /* Priority 6, above configMAX_SYSCALL_INTERRUPT_PRIORITY (5), so this
     * ISR may use the ISR-safe FreeRTOS API -- it posts an event. Same
     * choice as the GPIO interrupt in host_events.c, for the same reason. */
    nvic_irq_enable(u->irqn, 6, 0);
    u->open = true;
    return true;
}

void host_uart_release(int instance)
{
    uart_bus_t *u = uart_lookup(instance);
    if (u == NULL || !u->open) {
        return;
    }
    nvic_irq_disable(u->irqn);
    usart_interrupt_enable(u->periph, USART_RDBF_INT, FALSE);
    usart_enable(u->periph, FALSE);
    crm_periph_clock_enable(u->clock, FALSE);
    u->open = false;
    u->head = u->tail = 0;
}

uint32_t host_uart_dropped(int instance)
{
    uart_bus_t *u = uart_lookup(instance);
    return (u != NULL) ? u->dropped : 0u;
}

/* ---- the module-facing half ------------------------------------------ */

static uart_bus_t *checked_uart(int instance)
{
    if (g_mdl_slot.state == MDL_SLOT_EMPTY) {
        return NULL;
    }
    bool declared = false;
    for (uint8_t i = 0; i < g_mdl_slot.res_count; i++) {
        if (g_mdl_slot.res[i].kind == (uint8_t)MDL_RES_KIND_UART &&
            g_mdl_slot.res[i].id == (uint8_t)instance) {
            declared = true;
            break;
        }
    }
    if (!declared) {
        return NULL;
    }
    uart_bus_t *u = uart_lookup(instance);
    return (u != NULL && u->open) ? u : NULL;
}

/*
 * Half-duplex: TX and RX share the TX pin.
 *
 * DOES NOT WORK AS A LOOPBACK SELF-TEST ON THIS PART, and the attempt is
 * recorded because the idea is an obvious one to have twice.
 *
 * The intent was to prove the receive path without external wiring by
 * having the peripheral hear its own output. Measured on hardware: three
 * bytes sent, zero received, and the ring's dropped counter also zero --
 * so the receiver was not merely mis-framing, it was never triggered.
 * That was true with the TX pin push-pull and again with it open-drain
 * and pulled up, which is the configuration single-wire half duplex
 * requires.
 *
 * The likely reason is that this USART disconnects its receiver while
 * transmitting in single-wire mode, which is what most parts do -- the
 * point of the mode is talking to a device on a shared wire, not
 * listening to yourself. That is a guess about undocumented-here silicon
 * behaviour, so it is written as one.
 *
 * The function is kept because half duplex is a real thing an MDL might
 * need for a one-wire bus, and it is correctly implemented for that. It
 * is simply not a way to test reception. Proving the receive path needs a
 * signal from outside: a wire between PA2 and PA3, or a real serial
 * device on the pins.
 */
int host_uart_loopback_impl(int bus, int enable)
{
    uart_bus_t *u = checked_uart(bus);
    if (u == NULL) {
        return -1;
    }
    /* Single-wire half duplex needs the TX pin OPEN-DRAIN with a pull-up.
     * A push-pull output drives the line continuously in both directions,
     * so the receiver never sees the transitions the transmitter is
     * making -- which is precisely the symptom the first attempt showed:
     * sent 3, received 0. Push-pull is right for normal full duplex,
     * where TX and RX are different pins and only one of them is driven.
     */
    gpio_init_type gpio_init_struct;
    gpio_default_para_init(&gpio_init_struct);
    gpio_init_struct.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
    gpio_init_struct.gpio_mode           = GPIO_MODE_MUX;
    gpio_init_struct.gpio_pins           = u->tx_pin;
    gpio_init_struct.gpio_out_type = enable ? GPIO_OUTPUT_OPEN_DRAIN
                                             : GPIO_OUTPUT_PUSH_PULL;
    gpio_init_struct.gpio_pull     = enable ? GPIO_PULL_UP : GPIO_PULL_NONE;
    gpio_init(u->port, &gpio_init_struct);

    usart_enable(u->periph, FALSE);
    usart_single_line_halfduplex_select(u->periph, enable ? TRUE : FALSE);
    usart_enable(u->periph, TRUE);
    u->head = u->tail = 0;
    u->dropped = 0;
    return 0;
}

int host_uart_config_impl(int bus, uint32_t baud)
{
    uart_bus_t *u = checked_uart(bus);
    if (u == NULL) {
        return -1;
    }
    /* A baud rate the divisor cannot express would silently become some
     * other rate, which looks like a wiring fault from both ends. */
    if (baud < 1200u || baud > 3000000u) {
        return -1;
    }
    u->baud = baud;
    uart_apply(u);
    return 0;
}

int host_uart_write_impl(int bus, const void *data, uint32_t len)
{
    uart_bus_t *u = checked_uart(bus);
    if (u == NULL) {
        return -1;
    }
    if (len == 0u || len > UART_MAX_WRITE) {
        return -1;
    }
    if (!host_ptr_owned_by_module(data, len)) {
        return -1;
    }

    const uint8_t *p = (const uint8_t *)data;
    for (uint32_t i = 0; i < len; i++) {
        uint32_t spin = UART_TX_TIMEOUT_LOOPS;
        while (usart_flag_get(u->periph, USART_TDBE_FLAG) == RESET) {
            if (--spin == 0u) {
                /* Bounded because a UART whose CTS is held, or whose clock
                 * is misconfigured, would otherwise hang the module task
                 * forever on the first byte. */
                return -2;
            }
        }
        usart_data_transmit(u->periph, p[i]);
    }
    return 0;
}

int host_uart_read_impl(int bus, void *data, uint32_t maxlen)
{
    uart_bus_t *u = checked_uart(bus);
    if (u == NULL) {
        return -1;
    }
    if (maxlen == 0u || maxlen > UART_MAX_WRITE) {
        return -1;
    }
    if (!host_ptr_owned_by_module(data, maxlen)) {
        return -1;
    }

    uint8_t *out = (uint8_t *)data;
    uint32_t n = 0;
    /* Only tail moves here, and only head moves in the ISR, so this needs
     * no critical section -- the single-producer/single-consumer rule is
     * what buys that, and it is why neither side may ever touch the
     * other's index. */
    while (n < maxlen && u->tail != u->head) {
        out[n++] = u->ring[u->tail];
        u->tail = (uint16_t)((u->tail + 1u) % UART_RX_RING);
    }
    return (int)n;
}
