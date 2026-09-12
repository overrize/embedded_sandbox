/*
 * UART from inside the sandbox [ABI v8]: transmit, event-driven receive.
 *
 * HOW RECEPTION IS TESTED HERE: the SW3 button IS the signal source.
 *
 * PA3 is USART2's RX and also the SW3 button, and PA3 is not brought out
 * to a header on this board -- so there is no jumper to run and no second
 * device to attach. Pressing SW3 pulls the receive line low, which is a
 * start bit, and that is the only signal this board can put on that pin.
 *
 * The baud rate is what makes it work, and 1200 is chosen, not inherited:
 * one bit lasts 833us there, so a press is measured in bit times rather
 * than in hundreds of them.
 *
 *   a very short tap (~2-3ms)  start bit, a few low data bits, then the
 *                              line is high again for the stop bit --
 *                              FRAMES AS A REAL BYTE, and the whole chain
 *                              runs: ISR, ring, event, module_event()
 *   a normal press (30ms+)     line still low where the stop bit belongs
 *                              -- a FRAMING ERROR, which the host counts
 *
 * Both outcomes are evidence, which is the point. What was measured
 * before -- zero bytes AND zero errors -- means the receiver was never
 * triggered at all, and that is the only result that says the path is
 * broken. An earlier version of this comment predicted that a mechanical
 * edge was simply too slow to frame; that reasoning was done at 9600 baud
 * and never actually run with a finger on the button.
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
    if (host->uart_config(2, 1200) != 0) {
        host->log("uart_echo: uart_config refused -- is USART2 declared?");
        return -1;
    }

    /* 1200, so a button press lands in the right order of magnitude
     * against the 833us bit period. The host refuses anything below 1200,
     * so this is as slow as the receiver can be made without a reflash --
     * and it is slow enough. */
    const char hello[] = "MDL uart alive\r\n";
    int w = host->uart_write(2, hello, sizeof(hello) - 1);
    if (w == -2) {
        host->log("uart_echo: transmit timed out -- clock or pin mux wrong");
    } else if (w != 0) {
        host->log("uart_echo: transmit refused");
    } else {
        host->log("uart_echo: 1200 baud. PRESS SW3 -- short taps frame as bytes, long presses as errors. Then run `uart`.");
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
     * PA3 is not brought out to a header on this board, so there is no
     * jumper to run -- an earlier version of this comment suggested one,
     * which was advice for a board this is not. The SW3 button is the only
     * signal source PA3 has, which is why the baud rate above is set for
     * it rather than for a serial peer. */
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
                    : "  -- expected on this part; press SW3 instead");
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
