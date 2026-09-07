/*
 * Resource arbitration, in the space where it is actually decidable:
 * physical pins.
 *
 * Comparing claim NAMES cannot work. "USART2" and "GPIO 2" look unrelated
 * and are the same copper on this board -- USART2_RX and the SW3 button
 * are both PA3. Every claim is therefore expanded to the pins it occupies
 * and the pins are what get compared.
 *
 * Two things are refused here:
 *   - a pin the host holds HARD, i.e. one it cannot give up and still be
 *     a working host (the debug UART, the USB data lines);
 *   - two claims by the same MDL landing on one pin.
 *
 * The second is decidable purely from the image, so it belongs in the
 * packer as well (maintain.md B2) -- failing the build beats failing the
 * load. This is the last line of defence, not the first.
 *
 * A pin the host holds only by courtesy (the indicator LEDs) is granted
 * and the host stands down; see main.c's indicator_task.
 */
#include "host_api.h"
#include "registry.h"
#include "mdl_format.h"

/* ---- what the host holds ------------------------------------------- */

typedef enum {
    PIN_FREE = 0,
    PIN_HOST_YIELDS,
    PIN_HOST_HARD,
} pin_hold_t;

typedef struct {
    uint8_t     pin;
    pin_hold_t  how;
    const char *who;
} pin_owner_t;

static const pin_owner_t g_pin_owner[] = {
    { MDL_PIN(0,  9), PIN_HOST_HARD,   "the DAP debug UART, PA9 TX"  },
    { MDL_PIN(0, 10), PIN_HOST_HARD,   "the DAP debug UART, PA10 RX" },
    { MDL_PIN(0, 11), PIN_HOST_HARD,   "USB OTGFS1 D-, PA11"         },
    { MDL_PIN(0, 12), PIN_HOST_HARD,   "USB OTGFS1 D+, PA12"         },
    { MDL_PIN(3, 10), PIN_HOST_YIELDS, "the host alive-blink LED"    },
    { MDL_PIN(4, 15), PIN_HOST_YIELDS, "the host USB-link LED"       },
};
#define PIN_OWNER_COUNT (sizeof(g_pin_owner) / sizeof(g_pin_owner[0]))

/* ---- which pins each peripheral occupies ---------------------------- */

/*
 * Chip facts, taken from the vendor's own examples rather than from
 * memory:
 *   I2C1   PB6/PB7   examples/i2c/communication_int  (GPIO_MUX_4)
 *   USART2 PA2/PA3   examples/usart/idle_detection   (GPIO_MUX_7)
 *   TMR3   PA6/PA7   examples/tmr/cascade_synchro, input_capture (MUX_2)
 *
 * TMR3 CH3/CH4 on PB0/PB1 come from the alternate-function table and are
 * NOT confirmed against a vendor example. Flagged rather than quietly
 * included, because a wrong row here produces a conflict report that is
 * confidently incorrect -- worse than a missing one.
 *
 * Deliberately short. Transcribing every alternate function from the
 * datasheet would be hundreds of rows with no way to spot a typo; this
 * grows as peripherals are actually used.
 */
typedef struct {
    uint8_t     kind;
    uint8_t     id;
    const char *name;
    uint8_t     pins[4];
    uint8_t     npins;
} periph_pins_t;

static const periph_pins_t g_periph[] = {
    { MDL_RES_KIND_I2C,   1, "I2C1 (PB6 SCL, PB7 SDA)",
      { MDL_PIN(1, 6), MDL_PIN(1, 7) }, 2 },
    { MDL_RES_KIND_UART,  1, "USART1 (PA9 TX, PA10 RX)",
      { MDL_PIN(0, 9), MDL_PIN(0, 10) }, 2 },
    { MDL_RES_KIND_UART,  2, "USART2 (PA2 TX, PA3 RX)",
      { MDL_PIN(0, 2), MDL_PIN(0, 3) }, 2 },
    { MDL_RES_KIND_TIMER, 3, "TMR3 (PA6 CH1, PA7 CH2, PB0 CH3, PB1 CH4)",
      { MDL_PIN(0, 6), MDL_PIN(0, 7), MDL_PIN(1, 0), MDL_PIN(1, 1) }, 4 },
};
#define PERIPH_COUNT (sizeof(g_periph) / sizeof(g_periph[0]))

static const periph_pins_t *periph_lookup(uint8_t kind, uint8_t id)
{
    for (unsigned i = 0; i < PERIPH_COUNT; i++) {
        if (g_periph[i].kind == kind && g_periph[i].id == id) {
            return &g_periph[i];
        }
    }
    return NULL;
}

static const pin_owner_t *pin_owner(uint8_t pin)
{
    for (unsigned i = 0; i < PIN_OWNER_COUNT; i++) {
        if (g_pin_owner[i].pin == pin) {
            return &g_pin_owner[i];
        }
    }
    return NULL;
}

/*
 * Expand one claim into the pins it occupies. Returns 0 for a claim this
 * board does not know how to place -- refused rather than assumed free,
 * because "I don't recognise it" is not evidence that it is available.
 */
static uint8_t res_pins(const mdl_res_t *r, uint8_t *out, const char **name)
{
    if (r->kind == (uint8_t)MDL_RES_KIND_GPIO) {
        uint8_t pin;
        if (!host_gpio_pin_id((int)r->id, &pin)) {
            return 0;
        }
        out[0] = pin;
        *name  = host_gpio_name((int)r->id);
        return 1;
    }

    const periph_pins_t *p = periph_lookup(r->kind, r->id);
    if (p == NULL) {
        return 0;
    }
    for (uint8_t i = 0; i < p->npins; i++) {
        out[i] = p->pins[i];
    }
    *name = p->name;
    return p->npins;
}

/* ---- message building (no printf on this target) --------------------- */

static char *put_str(char *w, const char *end, const char *t)
{
    while (t != NULL && *t != 0 && w < end - 1) {
        *w++ = *t++;
    }
    return w;
}

static char *put_pin(char *w, const char *end, uint8_t pin)
{
    if (w < end - 5) {
        *w++ = 'P';
        *w++ = (char)('A' + MDL_PIN_PORT(pin));
        uint8_t n = MDL_PIN_NUM(pin);
        if (n >= 10u) {
            *w++ = '1';
            n = (uint8_t)(n - 10u);
        }
        *w++ = (char)('0' + n);
    }
    return w;
}

static const char *kind_name(uint8_t kind)
{
    switch (kind) {
    case MDL_RES_KIND_GPIO:  return "gpio";
    case MDL_RES_KIND_I2C:   return "i2c";
    case MDL_RES_KIND_UART:  return "uart";
    case MDL_RES_KIND_TIMER: return "timer";
    default:                  return "unknown";
    }
}

bool mdl_res_check(const mdl_res_t *res, uint32_t count,
                    char *detail, uint32_t detail_len)
{
    /* Every pin claimed so far, and by which claim, so a collision can be
     * reported as "X and Y are both PA3" rather than just "conflict". */
    uint8_t     seen[MDL_MAX_RES * 4];
    const char *seen_by[MDL_MAX_RES * 4];
    uint8_t     nseen = 0;

    char       *w   = detail;
    const char *end = detail + detail_len;
    detail[0] = 0;

    for (uint32_t i = 0; i < count; i++) {
        uint8_t     pins[4];
        const char *name = "(unnamed)";
        uint8_t     n = res_pins(&res[i], pins, &name);

        if (n == 0u) {
            w = put_str(w, end, kind_name(res[i].kind));
            w = put_str(w, end, " instance not known on this board");
            *w = 0;
            return false;
        }

        for (uint8_t k = 0; k < n; k++) {
            const pin_owner_t *o = pin_owner(pins[k]);
            if (o != NULL && o->how == PIN_HOST_HARD) {
                w = put_str(w, end, name);
                w = put_str(w, end, " needs ");
                w = put_pin(w, end, pins[k]);
                w = put_str(w, end, ", held by ");
                w = put_str(w, end, o->who);
                *w = 0;
                return false;
            }

            for (uint8_t j = 0; j < nseen; j++) {
                if (seen[j] == pins[k]) {
                    w = put_str(w, end, name);
                    w = put_str(w, end, " and ");
                    w = put_str(w, end, seen_by[j]);
                    w = put_str(w, end, " are both ");
                    w = put_pin(w, end, pins[k]);
                    *w = 0;
                    return false;
                }
            }
            if (nseen < (uint8_t)(MDL_MAX_RES * 4)) {
                seen[nseen]      = pins[k];
                seen_by[nseen++] = name;
            }
        }
    }
    return true;
}
