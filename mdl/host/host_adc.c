/*
 * ADC for MDLs.
 *
 * Shaped differently from both I2C and UART, because of one fact: an ADC is
 * ONE peripheral with MANY channels, not many instances. So the resource id
 * here is a CHANNEL, not an instance -- MDL_RES_ADC(4) claims channel 4,
 * which the host expands to PA4, and pin-level arbitration then refuses any
 * other claim on that pin. That is exactly what R3's pin expansion exists
 * for: two declarations with different names landing on one wire.
 *
 * The practical consequence is good: a single firmware can offer every
 * channel at once, so there is no guessing about which pin a given board
 * brings out to a header. That lesson came the hard way from USART2, where
 * the whole receive path was untestable because its RX pin (PA3) has no
 * header -- three rounds were spent working around a pin instead of picking
 * a different one.
 *
 * MILLIVOLTS WITHOUT ASSUMING THE RAIL. adc_read() returns raw counts, and
 * adc_read_mv() converts using the chip's internal 1.2V reference on
 * channel 17 rather than a hard-coded 3.3V:
 *
 *     mV = raw * 1200 / reference_counts
 *
 * VDDA cancels out of that expression entirely. Assuming 3.3V on a board
 * actually running 3.28V is a silent 0.6% error in every reading, and a
 * calibration constant nobody can see is worse than no calibration.
 */
#include "host_api.h"
#include "registry.h"
#include "mdl_format.h"
#include "at32f435_437.h"
#include "console.h"
#include "FreeRTOS.h"
#include "task.h"

/* Conversion is microseconds at this sample time; a bound this loose only
 * ever trips if the peripheral is not actually running. */
#define ADC_POLL_LIMIT 200000u

/* The internal 1.2V bandgap, per the vendor's current_vref_value_check
 * example. Not a channel an MDL may claim -- it is the host's instrument. */
#define ADC_VREF_CHANNEL  ADC_CHANNEL_17
#define ADC_VREF_MV       1200u
#define ADC_FULL_SCALE    4095u

typedef struct {
    uint8_t     channel;      /* = the resource id an MDL declares */
    gpio_type  *port;
    uint16_t    pin;
    crm_periph_clock_type gpio_clock;
    bool        confirmed;    /* pin mapping seen in a vendor example? */
    bool        claimed;
} adc_ch_t;

/*
 * Channels offered, and the pins they are.
 *
 * PA4/PA5/PA6 -> CH4/CH5/CH6 are confirmed against the vendor's
 * use_polling_get_conversion_data example, which establishes the
 * PA(n) -> CH(n) pattern for the whole PA0..PA7 block.
 *
 * PC0..PC5 -> CH10..CH15 comes from the datasheet pin table and has NOT
 * been confirmed against an example -- flagged the same way TMR3's CH3/CH4
 * is flagged in board_pins.def, because a channel that silently samples the
 * wrong pin would present as "my sensor does nothing".
 *
 * Deliberately absent: CH2/CH3 (PA2/PA3 are USART2), CH6/CH7 (PA6/PA7 are
 * TMR3 CH1/CH2), CH8/CH9 (PB0/PB1 are TMR3 CH3/CH4). Offering a channel
 * whose pin another peripheral in board_pins.def already owns would mean
 * shipping a guaranteed conflict.
 */
static adc_ch_t g_chans[] = {
    { 0,  GPIOA, GPIO_PINS_0, CRM_GPIOA_PERIPH_CLOCK, true,  false },
    { 1,  GPIOA, GPIO_PINS_1, CRM_GPIOA_PERIPH_CLOCK, true,  false },
    { 4,  GPIOA, GPIO_PINS_4, CRM_GPIOA_PERIPH_CLOCK, true,  false },
    { 5,  GPIOA, GPIO_PINS_5, CRM_GPIOA_PERIPH_CLOCK, true,  false },
    { 10, GPIOC, GPIO_PINS_0, CRM_GPIOC_PERIPH_CLOCK, false, false },
    { 11, GPIOC, GPIO_PINS_1, CRM_GPIOC_PERIPH_CLOCK, false, false },
    { 12, GPIOC, GPIO_PINS_2, CRM_GPIOC_PERIPH_CLOCK, false, false },
    { 13, GPIOC, GPIO_PINS_3, CRM_GPIOC_PERIPH_CLOCK, false, false },
    { 14, GPIOC, GPIO_PINS_4, CRM_GPIOC_PERIPH_CLOCK, false, false },
    { 15, GPIOC, GPIO_PINS_5, CRM_GPIOC_PERIPH_CLOCK, false, false },
};
#define CHAN_COUNT (sizeof(g_chans) / sizeof(g_chans[0]))

static bool s_adc_up;

static adc_ch_t *chan_lookup(int channel)
{
    for (unsigned i = 0; i < CHAN_COUNT; i++) {
        if (g_chans[i].channel == (uint8_t)channel) {
            return &g_chans[i];
        }
    }
    return NULL;
}

static void adc_bring_up(void)
{
    if (s_adc_up) {
        return;
    }
    crm_periph_clock_enable(CRM_ADC1_PERIPH_CLOCK, TRUE);

    adc_common_config_type common;
    adc_common_default_para_init(&common);
    common.combine_mode          = ADC_INDEPENDENT_MODE;
    common.div                   = ADC_HCLK_DIV_4;
    common.sampling_interval     = ADC_SAMPLING_INTERVAL_5CYCLES;
    /* Powers the temperature sensor and the internal voltage reference.
     * Without it channel 17 reads noise and every millivolt figure derived
     * from it would be confidently wrong. */
    common.tempervintrv_state    = TRUE;
    common.vbat_state            = FALSE;
    adc_common_config(&common);

    adc_base_config_type base;
    adc_base_default_para_init(&base);
    /* Single channel, single shot, no sequence: each read names its own
     * channel. Repeat mode would keep converting the last channel set,
     * which is the wrong shape for "read this pin now". */
    base.sequence_mode           = FALSE;
    base.repeat_mode             = FALSE;
    base.data_align              = ADC_RIGHT_ALIGNMENT;
    base.ordinary_channel_length = 1;
    adc_base_config(ADC1, &base);

    adc_resolution_set(ADC1, ADC_RESOLUTION_12B);
    adc_ordinary_conversion_trigger_set(ADC1, ADC_ORDINARY_TRIG_TMR1CH1,
                                         ADC_ORDINARY_TRIG_EDGE_NONE);

    adc_enable(ADC1, TRUE);
    while (adc_flag_get(ADC1, ADC_RDY_FLAG) == RESET) { }

    /* Calibration is not optional: skipping it costs several LSB of offset
     * and gain error, which is exactly the size of the effect anyone
     * measuring a sensor is trying to see. */
    adc_calibration_init(ADC1);
    while (adc_calibration_init_status_get(ADC1)) { }
    adc_calibration_start(ADC1);
    while (adc_calibration_status_get(ADC1)) { }

    s_adc_up = true;
}

/* One conversion on one channel. Returns -1 if the peripheral never
 * reported completion, which means it is not running rather than that the
 * pin reads zero -- those must not look the same to the caller. */
static int convert_once(uint8_t adc_channel)
{
    adc_ordinary_channel_set(ADC1, (adc_channel_select_type)adc_channel, 1,
                              ADC_SAMPLETIME_640_5);
    adc_flag_clear(ADC1, ADC_OCCE_FLAG);
    adc_ordinary_software_trigger_enable(ADC1, TRUE);

    uint32_t spin = ADC_POLL_LIMIT;
    while (adc_flag_get(ADC1, ADC_OCCE_FLAG) == RESET) {
        if (--spin == 0u) {
            adc_ordinary_software_trigger_enable(ADC1, FALSE);
            return -1;
        }
    }
    adc_ordinary_software_trigger_enable(ADC1, FALSE);
    return (int)adc_ordinary_conversion_data_get(ADC1);
}

bool host_adc_claim(int channel)
{
    adc_ch_t *c = chan_lookup(channel);
    if (c == NULL) {
        mdl_console_puts("[host] adc: no such channel on this board\r\n");
        return false;
    }
    if (c->claimed) {
        return true;
    }

    if (!c->confirmed) {
        /* Allowed, but said out loud. The pin might be right; it has simply
         * not been proven, and a reading from an unproven mapping deserves
         * to be read with that in mind. */
        mdl_console_puts("[host] adc: channel pin mapping not example-confirmed\r\n");
    }

    crm_periph_clock_enable(c->gpio_clock, TRUE);

    gpio_init_type gpio_init_struct;
    gpio_default_para_init(&gpio_init_struct);
    /* Analog mode disconnects the digital input buffer. Leaving it
     * connected both loads the signal and burns current when the input
     * sits near mid-rail. */
    gpio_init_struct.gpio_mode = GPIO_MODE_ANALOG;
    gpio_init_struct.gpio_pull = GPIO_PULL_NONE;
    gpio_init_struct.gpio_pins = c->pin;
    gpio_init(c->port, &gpio_init_struct);

    adc_bring_up();
    c->claimed = true;
    return true;
}

void host_adc_release(int channel)
{
    adc_ch_t *c = chan_lookup(channel);
    if (c == NULL || !c->claimed) {
        return;
    }

    /* Back to a high-impedance digital input: it drives nothing and holds
     * nothing, which is the safe state for a pin whose next owner is
     * unknown. This is R1's rule -- whatever a module configured, the host
     * puts back. */
    gpio_init_type gpio_init_struct;
    gpio_default_para_init(&gpio_init_struct);
    gpio_init_struct.gpio_mode = GPIO_MODE_INPUT;
    gpio_init_struct.gpio_pull = GPIO_PULL_NONE;
    gpio_init_struct.gpio_pins = c->pin;
    gpio_init(c->port, &gpio_init_struct);
    c->claimed = false;

    for (unsigned i = 0; i < CHAN_COUNT; i++) {
        if (g_chans[i].claimed) {
            return;   /* another channel still needs the peripheral */
        }
    }
    adc_enable(ADC1, FALSE);
    crm_periph_clock_enable(CRM_ADC1_PERIPH_CLOCK, FALSE);
    s_adc_up = false;
}

/* ---- the module-facing half ------------------------------------------ */

static adc_ch_t *checked_chan(int channel)
{
    if (g_mdl_slot.state == MDL_SLOT_EMPTY) {
        return NULL;
    }
    bool declared = false;
    for (uint8_t i = 0; i < g_mdl_slot.res_count; i++) {
        if (g_mdl_slot.res[i].kind == (uint8_t)MDL_RES_KIND_ADC &&
            g_mdl_slot.res[i].id == (uint8_t)channel) {
            declared = true;
            break;
        }
    }
    if (!declared) {
        return NULL;
    }
    adc_ch_t *c = chan_lookup(channel);
    return (c != NULL && c->claimed && s_adc_up) ? c : NULL;
}

int host_adc_read_impl(int channel)
{
    adc_ch_t *c = checked_chan(channel);
    if (c == NULL) {
        return -1;
    }
    int raw = convert_once(c->channel);
    return (raw < 0) ? -2 : raw;
}

int host_adc_read_mv_impl(int channel)
{
    adc_ch_t *c = checked_chan(channel);
    if (c == NULL) {
        return -1;
    }

    int raw = convert_once(c->channel);
    if (raw < 0) {
        return -2;
    }

    /* Read the internal reference in the same breath as the signal, so a
     * drifting or sagging rail is divided out rather than recorded as a
     * change in the thing being measured. */
    int ref = convert_once((uint8_t)ADC_VREF_CHANNEL);
    if (ref <= 0) {
        return -2;
    }

    /* mV = raw * (VDDA / full_scale), and VDDA = 1200 * full_scale / ref,
     * so full scale and VDDA both cancel: mV = raw * 1200 / ref. Nothing
     * here assumes what the rail actually is. */
    return (int)(((uint32_t)raw * ADC_VREF_MV) / (uint32_t)ref);
}

/* For the `adc` console command: what the host can offer, regardless of
 * what is loaded. Lets someone ask "where do I connect?" without first
 * writing an MDL. */
int host_adc_describe(int index, int *channel, char *port, int *pin, bool *confirmed)
{
    if (index < 0 || (unsigned)index >= CHAN_COUNT) {
        return -1;
    }
    adc_ch_t *c = &g_chans[index];
    *channel   = c->channel;
    *port      = (c->port == GPIOA) ? 'A' : 'C';
    *confirmed = c->confirmed;
    for (int b = 0; b < 16; b++) {
        if (c->pin == (uint16_t)(1u << b)) {
            *pin = b;
            break;
        }
    }
    return (int)CHAN_COUNT;
}
