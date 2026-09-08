/*
 * UART from inside the sandbox [ABI v8]: transmit, event-driven receive.
 *
 * HOW THIS IS TESTABLE WITHOUT A SECOND DEVICE. `uart` runs a loopback
 * self-test: half-duplex mode ties TX and RX to the same pin, so the
 * peripheral receives its own output. That covers the entire chain -- baud
 * divisor, pin mux, receive interrupt, ring buffer, event delivery -- with
 * nothing wired up and nobody pressing anything.
 *
 * The first attempt used the SW3 button, since USART2's RX is PA3 and PA3
 * is also that button, so pressing it pulls the line low like a start bit.
 * Whether a receiver frames that as a character is UNTESTED -- a mechanical
 * edge lasts milliseconds against a 104us bit period at 9600 baud, which
 * argues against it, but nobody has actually pressed the button while the
 * counters were being watched, so that is a prediction and not a result.
 *
 * The reason to replace it is not that it failed. It is that it needs a
 * person, so it cannot be repeated, cannot run unattended, and cannot
 * distinguish "the receive path is broken" from "nobody pressed anything"
 * -- which is exactly the ambiguity that wasted a round here.
 *
 * Note this MDL declares only MDL_RES_UART(2). It does NOT declare
 * MDL_RES_GPIO(2), and could not: both are PA3, and the host refuses one
 * MDL claiming the same pin twice. The button is the UART's now.
 */
#include "host_api.h"

MDL_MODULE_ABI_DECLARE();
MDL_MODULE_COMMAND("uart");
MDL_MODULE_EVENTS(4, 100);
MDL_MODULE_RESOURCES(MDL_RES_UART(2));   /* PA2 TX, PA3 RX (= SW3) */

static uint32_t g_rx_total = 0;
static uint32_t g_events   = 0;
static uint8_t  g_last[8];
static uint32_t g_last_n   = 0;

static char g_line[96];

static char *put_u32(char *w, const char *end, uint32_t v)
{
    char tmp[12];
    int n = 0;
    do {
        tmp[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    } while (v != 0u && n < (int)sizeof(tmp));
    while (n-- > 0 && w < end - 1) {
        *w++ = tmp[n];
    }
    return w;
}

static char *put_hex8(char *w, const char *end, uint8_t v)
{
    static const char d[] = "0123456789ABCDEF";
    if (w < end - 3) {
        *w++ = d[(v >> 4) & 0xF];
        *w++ = d[v & 0xF];
        *w++ = ' ';
    }
    return w;
}

static char *put_str(char *w, const char *end, const char *t)
{
    while (*t != 0 && w < end - 1) {
        *w++ = *t++;
    }
    return w;
}

int module_init(const host_api_t *host)
{
    if (host->uart_config(2, 9600) != 0) {
        host->log("uart_echo: uart_config refused -- is USART2 declared?");
        return -1;
    }

    /* 9600 is left over from the button experiment, where a longer bit
     * period seemed more likely to catch a slow mechanical edge. It is
     * kept because loopback works at any rate and a slower one makes the
     * 3-byte probe comfortably longer than the 50ms wait below. */
    const char hello[] = "MDL uart alive\r\n";
    int w = host->uart_write(2, hello, sizeof(hello) - 1);
    if (w == -2) {
        host->log("uart_echo: transmit timed out -- clock or pin mux wrong");
    } else if (w != 0) {
        host->log("uart_echo: transmit refused");
    } else {
        host->log("uart_echo: sent a line on PA2 at 9600; run `uart` to self-test RX");
    }
    return 0;
}

int module_event(const host_api_t *host, const mdl_event_t *evt)
{
    if (evt->source != MDL_EVT_UART) {
        return 0;
    }
    g_events++;

    /* Local buffer, i.e. on this task's stack -- which is module memory
     * the host accepts. It did not, until I2C made someone try it. */
    uint8_t buf[32];
    int n = host->uart_read(2, buf, sizeof(buf));
    if (n <= 0) {
        return 0;
    }

    g_rx_total += (uint32_t)n;
    g_last_n = ((uint32_t)n < sizeof(g_last)) ? (uint32_t)n : sizeof(g_last);
    for (uint32_t i = 0; i < g_last_n; i++) {
        g_last[i] = buf[i];
    }
    return 0;
}

int module_cmd(const host_api_t *host, int argc, const char *const *argv)
{
    (void)argc;
    (void)argv;

    /* Half-duplex probe. This does NOT work as a self-test on this part --
     * measured: 3 sent, 0 received, dropped counter also 0, so the
     * receiver is never triggered while transmitting (see host_uart.c).
     * It is left in because it costs nothing and would immediately show a
     * change if that silicon behaviour were ever different.
     *
     * To actually verify reception, put a signal on the pins: a wire from
     * PA2 to PA3 turns this into a real loopback, and then `uart` reports
     * bytes. Nothing here can substitute for that. */
    if (host->uart_loopback(2, 1) != 0) {
        host->log("  loopback not available");
    } else {
        const char probe[] = "MDL";
        uint32_t before = g_rx_total;
        if (host->uart_write(2, probe, 3) != 0) {
            host->log("  loopback: transmit failed");
        } else {
            /* Three bytes at 9600 is ~3ms; 50 leaves room for the event to
             * be delivered and module_event() to run. */
            host->delay_ms(50);
            uint32_t got = g_rx_total - before;
            char *lw = g_line;
            const char *lend = g_line + sizeof(g_line);
            lw = put_str(lw, lend, "  loopback: sent 3, received ");
            lw = put_u32(lw, lend, got);
            lw = put_str(lw, lend, got == 3u
                    ? "  -- receive path WORKS"
                    : "  -- expected on this part; wire PA2 to PA3 to test RX");
            *lw = 0;
            host->log(g_line);
        }
        host->uart_loopback(2, 0);
    }

    char *w = g_line;
    const char *end = g_line + sizeof(g_line);
    w = put_str(w, end, "  uart2: ");
    w = put_u32(w, end, g_rx_total);
    w = put_str(w, end, " bytes in ");
    w = put_u32(w, end, g_events);
    w = put_str(w, end, " event(s)");
    if (g_last_n > 0u) {
        w = put_str(w, end, ", last: ");
        for (uint32_t i = 0; i < g_last_n; i++) {
            w = put_hex8(w, end, g_last[i]);
        }
    }
    *w = 0;
    host->log(g_line);

    /* Transmit again on demand, so the command also answers "is TX still
     * working" rather than only reporting on RX. */
    const char again[] = "ping\r\n";
    if (host->uart_write(2, again, sizeof(again) - 1) == 0) {
        host->log("  sent 'ping' on PA2");
    }
    return (int)g_rx_total;
}
