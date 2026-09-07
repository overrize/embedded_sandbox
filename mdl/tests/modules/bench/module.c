/*
 * Measures what the sandbox actually costs, from inside the sandbox.
 *
 * The project's central claim is "almost no performance cost" against an
 * interpreter. That has been an argument from architecture so far -- true
 * by construction, but not a number. These are the numbers, and they are
 * taken with host->cycles() (DWT CYCCNT, ~3.5ns per count at 288MHz)
 * because the RTOS tick is 1ms and would round every one of them to zero.
 *
 * Three things get measured, and they are not equally interesting:
 *
 *   1. PURE COMPUTATION. Should be indistinguishable from the same loop
 *      compiled into the firmware, because it IS the same instructions --
 *      an MDL is native ARM code, not bytecode. If this number is bad,
 *      the premise of the project is wrong.
 *
 *   2. ONE GATED HOST CALL. This is the tax: every vtable call is an SVC
 *      round trip through xPortRaisePrivilege/vPortResetPrivilege. It is
 *      what an MDL pays for not being trusted, and it is the number to
 *      watch when someone puts a host call in a hot loop.
 *
 *   3. EVENT DISPATCH (module_event below). Interrupt to handler: the ISR
 *      stamps evt->cycles, this reads the counter on entry. That gap is
 *      the price of running the handler unprivileged in a task instead of
 *      in the ISR -- the one cost that cannot be optimised away without
 *      giving up the isolation.
 *
 * Run it, press SW3 a few times, then type `bench` at the console for the
 * dispatch summary.
 *
 * The summary is accumulated rather than printed per event, which was the
 * first attempt and does not work: the console is shared and unbuffered,
 * so the interesting lines scroll past while nobody is listening. A
 * benchmark you have to be watching at the right moment is not a
 * measurement, it is a coincidence.
 */
#include "host_api.h"

MDL_MODULE_ABI_DECLARE();
MDL_MODULE_EVENTS(4, 50);
MDL_MODULE_COMMAND("bench");
MDL_MODULE_RESOURCES(MDL_RES_GPIO_IRQ(2, MDL_EDGE_BOTH));

#define ITERATIONS 1000u

static char g_line[64];

/* Accumulated across every event, so the numbers survive until asked for.
 * Non-zero initialisers keep these in real .data, which also makes them
 * visible in a `mem` dump of the arena. */
static uint32_t g_lat_min = 0xFFFFFFFFu;
static uint32_t g_lat_max = 0;
static uint32_t g_lat_sum = 0;
static uint32_t g_lat_n   = 0;

/* No stdlib in an MDL, so decimal formatting is ours to write. Appends and
 * returns the new end, so the callers below read like a print statement. */
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

static char *put_str(char *w, const char *end, const char *t)
{
    while (*t != 0 && w < end - 1) {
        *w++ = *t++;
    }
    return w;
}

/* Cycles x100 per iteration, so one decimal place survives integer maths --
 * the interesting numbers here are single digits and "3" vs "3.4" matters. */
static void report(const host_api_t *host, const char *what, uint32_t total)
{
    uint32_t hundredths = (total * 100u) / ITERATIONS;
    char *w = g_line;
    const char *end = g_line + sizeof(g_line);
    w = put_str(w, end, what);
    w = put_u32(w, end, hundredths / 100u);
    w = put_str(w, end, ".");
    w = put_u32(w, end, (hundredths % 100u) / 10u);
    w = put_u32(w, end, hundredths % 10u);
    w = put_str(w, end, " cycles");
    *w = 0;
    host->log(g_line);
}

int module_init(const host_api_t *host)
{
    host->log("bench: measuring from inside the sandbox");

    /*
     * 1. Pure computation. volatile so the compiler cannot delete the loop
     *    -- an optimised-away benchmark reports a wonderful zero.
     */
    volatile uint32_t acc = 0;
    uint32_t t0 = host->cycles();
    for (uint32_t i = 0; i < ITERATIONS; i++) {
        acc = acc + i;
    }
    uint32_t t1 = host->cycles();
    report(host, "  native loop iteration : ", t1 - t0);

    /*
     * 2. A gated host call. cycles() is itself gated, so the two reads
     *    bracketing the loop cost the same as one iteration -- immaterial
     *    over ITERATIONS, which is exactly why this is a loop and not a
     *    single call.
     */
    t0 = host->cycles();
    for (uint32_t i = 0; i < ITERATIONS; i++) {
        (void)host->gpio_get(2);
    }
    t1 = host->cycles();
    report(host, "  gated host call       : ", t1 - t0);

    /*
     * 3. The same loop with no call at all, so the difference above is
     *    attributable to the gate rather than to loop overhead.
     */
    t0 = host->cycles();
    for (uint32_t i = 0; i < ITERATIONS; i++) {
        acc = acc + 1u;
    }
    t1 = host->cycles();
    report(host, "  empty loop iteration  : ", t1 - t0);

    host->log("bench: now press SW3 -- each edge reports dispatch latency");
    return 0;
}

int module_event(const host_api_t *host, const mdl_event_t *evt)
{
    /* Read the counter first: everything after this line is measurement
     * overhead being attributed to the thing measured. */
    uint32_t now = host->cycles();

    if (evt->source != MDL_EVT_GPIO) {
        return 0;
    }

    uint32_t lat = now - evt->cycles;
    if (lat < g_lat_min) {
        g_lat_min = lat;
    }
    if (lat > g_lat_max) {
        g_lat_max = lat;
    }
    g_lat_sum += lat;
    g_lat_n++;
    (void)host;
    return 0;
}

int module_cmd(const host_api_t *host, int argc, const char *const *argv)
{
    (void)argc;
    (void)argv;

    if (g_lat_n == 0u) {
        host->log("bench: no edges yet -- press SW3, then run this again");
        return 0;
    }

    char *w = g_line;
    const char *end = g_line + sizeof(g_line);
    w = put_str(w, end, "  isr->handler n=");
    w = put_u32(w, end, g_lat_n);
    w = put_str(w, end, " min=");
    w = put_u32(w, end, g_lat_min);
    w = put_str(w, end, " avg=");
    w = put_u32(w, end, g_lat_sum / g_lat_n);
    w = put_str(w, end, " max=");
    w = put_u32(w, end, g_lat_max);
    w = put_str(w, end, " cyc");
    *w = 0;
    host->log(g_line);

    /* 288 cycles per microsecond at 288MHz -- the number people think in. */
    w = g_line;
    w = put_str(w, end, "  that is min=");
    w = put_u32(w, end, g_lat_min / 288u);
    w = put_str(w, end, "us avg=");
    w = put_u32(w, end, (g_lat_sum / g_lat_n) / 288u);
    w = put_str(w, end, "us max=");
    w = put_u32(w, end, g_lat_max / 288u);
    w = put_str(w, end, "us");
    *w = 0;
    host->log(g_line);

    return (int)(g_lat_sum / g_lat_n);
}
