/*
 * ADC from inside the sandbox [ABI v9].
 *
 * `adcread` samples one channel 16 times and reports min / avg / max in both
 * raw counts and millivolts. Sixteen samples rather than one because a
 * single reading cannot tell a steady voltage from a noisy one, and the
 * spread is usually the first thing worth knowing about an analog input:
 * a few counts of jitter is normal, hundreds means something is wrong with
 * the signal, the ground, or the pin.
 *
 * CHANNEL 4 = PA4, chosen because the PA(n) -> CH(n) mapping is confirmed
 * against the vendor's use_polling_get_conversion_data example, and because
 * PA4 is not claimed by anything else in board_pins.def. Run `adc` at the
 * console for the full list this board offers -- every channel is available
 * in one firmware, so nothing has to be reflashed to move to another pin.
 *
 * THE MILLIVOLT FIGURE DOES NOT ASSUME 3.3V. The host divides by a reading
 * of the chip's internal 1.2V reference, so VDDA cancels out. On a board
 * actually running 3.28V an assumed 3.3V would shift every number by 0.6%
 * -- small, invisible, and exactly the kind of error that makes a sensor
 * look slightly miscalibrated forever.
 */
#include "host_api.h"

MDL_MODULE_ABI_DECLARE();
MDL_MODULE_COMMAND("adcread");
MDL_MODULE_RESOURCES(MDL_RES_ADC(4));   /* PA4 */

#define ADC_CH      4
#define SAMPLES    16

static char g_line[112];

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

int module_init(const host_api_t *host)
{
    int v = host->adc_read(ADC_CH);
    if (v == -1) {
        host->log("adc_read: refused -- is MDL_RES_ADC(4) declared, and does "
                  "this firmware have the driver?");
        return -1;
    }
    if (v == -2) {
        host->log("adc_read: conversion never completed -- the ADC is not running");
        return -1;
    }
    host->log("adc_read: PA4 (channel 4) claimed; run `adcread`");
    return 0;
}

int module_cmd(const host_api_t *host, int argc, const char *const *argv)
{
    (void)argc;
    (void)argv;

    uint32_t sum = 0;
    int lo = 4096, hi = -1;

    for (int i = 0; i < SAMPLES; i++) {
        int v = host->adc_read(ADC_CH);
        if (v < 0) {
            /* Report the reason rather than a number. -2 in particular must
             * not be folded into a reading: 0 counts is a perfectly good
             * measurement of a grounded pin, and "the ADC is not running"
             * is not a measurement at all. */
            host->log(v == -1 ? "  refused -- channel not declared"
                               : "  conversion failed -- ADC not running");
            return v;
        }
        sum += (uint32_t)v;
        if (v < lo) { lo = v; }
        if (v > hi) { hi = v; }
    }

    uint32_t avg = sum / SAMPLES;

    /* Millivolts for the average only: it costs a second conversion each
     * time, and converting all 16 would measure the reference 16 times to
     * no purpose. */
    int mv = host->adc_read_mv(ADC_CH);

    char *w = g_line;
    const char *end = g_line + sizeof(g_line);
    w = put_str(w, end, "  PA4 ch4: raw min ");
    w = put_u32(w, end, (uint32_t)lo);
    w = put_str(w, end, " / avg ");
    w = put_u32(w, end, avg);
    w = put_str(w, end, " / max ");
    w = put_u32(w, end, (uint32_t)hi);
    w = put_str(w, end, "   spread ");
    w = put_u32(w, end, (uint32_t)(hi - lo));
    *w = 0;
    host->log(g_line);

    w = g_line;
    if (mv >= 0) {
        w = put_str(w, end, "           ");
        w = put_u32(w, end, (uint32_t)mv);
        w = put_str(w, end, " mV  (against the internal 1.2V reference, "
                             "not an assumed rail)");
    } else {
        w = put_str(w, end, "           millivolt conversion unavailable");
    }
    *w = 0;
    host->log(g_line);

    return (int)avg;
}
