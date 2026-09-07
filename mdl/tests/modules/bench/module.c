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
 * Run it, then press SW3 a few times for the third number.
 */
#include "host_api.h"

MDL_MODULE_ABI_DECLARE();
MDL_MODULE_EVENTS(4, 50);
MDL_MODULE_RESOURCES(MDL_RES_GPIO_IRQ(2, MDL_EDGE_BOTH));

#define ITERATIONS 1000u

static char g_line[64];

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

    char *w = g_line;
    const char *end = g_line + sizeof(g_line);
    w = put_str(w, end, "  isr -> handler       : ");
    w = put_u32(w, end, now - evt->cycles);
    w = put_str(w, end, " cycles (");
    /* 288 cycles per microsecond at 288MHz. */
    w = put_u32(w, end, (now - evt->cycles) / 288u);
    w = put_str(w, end, " us), coalesced=");
    w = put_u32(w, end, evt->coalesced);
    *w = 0;
    host->log(g_line);
    return 0;
}
