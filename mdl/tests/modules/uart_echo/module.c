/*
 * UART from inside the sandbox [ABI v8]: transmit, event-driven receive.
 *
 * HOW RECEPTION IS TESTED: one jumper across PB10 and PB11. Verified on
 * hardware -- "MDL" goes out on PB10 and comes back on PB11, byte for
 * byte, and the event path delivers it to module_event() as well.
 *
 * That is a true full-duplex loopback -- the transmitter drives one pin,
 * the receiver listens on another, and a wire joins them. It leans on no
 * silicon quirk, needs no second device, needs nobody pressing anything,
 * and anybody can repeat it.
 *
 * Getting here took three wrong turns, and why is worth keeping. The work
 * started on USART2 because that was the instance already in the table,
 * and USART2's RX is PA3 -- which on this board has no header and carries
 * the SW3 button. Everything after that was an attempt to test reception
 * through a pin that cannot receive: half-duplex loopback (the receiver is
 * not triggered while transmitting), then the button as a signal source (a
 * real signal, but one that needs a person, so a null result cannot be
 * told apart from nobody pressing).
 *
 * The question was never "how do I get a signal onto PA3". It was "which
 * USART has both pins on headers" -- and USART3 does.
 *
 * Pin-level arbitration still applies: MDL_RES_UART(3) expands on the host
 * side to PB10 and PB11, so an MDL that also claimed either of those as
 * plain GPIO is refused -- by pin, not by name.
 */
#include "host_api.h"

MDL_MODULE_ABI_DECLARE();
MDL_MODULE_COMMAND("uart");
MDL_MODULE_EVENTS(4, 100);
MDL_MODULE_RESOURCES(MDL_RES_UART(3));   /* PB10 TX, PB11 RX -- jumper them */

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
    if (host->uart_config(3, 9600) != 0) {
        host->log("uart_echo: uart_config refused -- is USART2 declared?");
        return -1;
    }

    /* 9600. With a real wire there is no reason to crawl -- the 1200 baud
     * of the button experiment existed only to stretch the bit period out
     * to where a mechanical press could be framed as a character. */
    const char hello[] = "MDL uart alive\r\n";
    int w = host->uart_write(3, hello, sizeof(hello) - 1);
    if (w == -2) {
        host->log("uart_echo: transmit timed out -- clock or pin mux wrong");
    } else if (w != 0) {
        host->log("uart_echo: transmit refused");
    } else {
        host->log("uart_echo: USART3 at 9600. Jumper PB10 to PB11, then run `uart`.");
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
    int n = host->uart_read(3, buf, sizeof(buf));
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

    /* Send a known pattern and read it straight back off the wire.
     *
     * READ DIRECTLY, DO NOT WAIT FOR THE EVENT. The first version of this
     * slept 50ms expecting module_event() to have run, and reported
     * "received 0" on a loopback that was working perfectly -- because
     * module_cmd() and module_event() are drained by the SAME resident
     * task (see F1), so an event cannot be delivered while this function
     * is the thing occupying that task. It was waiting on something its
     * own execution made impossible.
     *
     * The cumulative counters below were what exposed it: they showed
     * exactly 16 bytes, the length of the hello line sent from
     * module_init(), while this test insisted nothing had arrived. When
     * two numbers on the same screen disagree, one of them is measuring
     * the wrong thing.
     *
     * uart_read() does not block and reads the host's ring directly, which
     * is the right tool here and needs no event at all. */
    {
        const char probe[] = "MDL";
        uint8_t back[8];

        /* Drain anything already waiting, so the comparison below is about
         * this probe and not about earlier traffic. */
        (void)host->uart_read(3, back, sizeof(back));

        if (host->uart_write(3, probe, 3) != 0) {
            host->log("  transmit failed");
        } else {
            /* 3 bytes at 9600 is ~3.1ms on the wire. 20 is generous. */
            host->delay_ms(20);
            int got = host->uart_read(3, back, sizeof(back));
            bool match = (got == 3) && back[0] == 'M' && back[1] == 'D' && back[2] == 'L';

            char *lw = g_line;
            const char *lend = g_line + sizeof(g_line);
            lw = put_str(lw, lend, "  wire test: sent MDL, got back ");
            lw = put_u32(lw, lend, (uint32_t)(got < 0 ? 0 : got));
            lw = put_str(lw, lend, " byte(s) ");
            for (int i = 0; i < got && i < 8; i++) {
                lw = put_hex8(lw, lend, back[i]);
            }
            lw = put_str(lw, lend, match
                    ? " -- RECEIVE PATH VERIFIED, byte for byte"
                    : " -- no jumper between PB10 and PB11?");
            *lw = 0;
            host->log(g_line);
        }
    }

    char *w = g_line;
    const char *end = g_line + sizeof(g_line);
    w = put_str(w, end, "  uart3: ");
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
    if (host->uart_write(3, again, sizeof(again) - 1) == 0) {
        host->log("  sent 'ping' on PB10");
    }
    return (int)g_rx_total;
}
