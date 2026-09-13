/*
 * SPI for MDLs.
 *
 * The primitive here is a single full-duplex transfer, because that is what
 * SPI physically is: the bus shifts a bit out and a bit in on the same
 * clock edge, so every byte sent produces a byte received. Splitting it
 * into write() and read() the way I2C is split would be inventing a
 * distinction the wire does not have, and would leave callers unable to
 * express the common case -- send a command and receive the answer to the
 * PREVIOUS one -- at all.
 *
 * CHIP SELECT IS NOT THIS DRIVER'S BUSINESS. An MDL that needs CS declares
 * a GPIO and toggles it around the transfer. That is deliberate: CS timing
 * is per-device (some parts want it held across several transfers, some
 * want a pulse between each, some use it to frame a command), and any
 * policy baked in here would be wrong for a third of them. Leaving it as a
 * pin also means the existing arbitration covers it with no new mechanism.
 *
 * Pins are PB3 SCK / PB4 MISO / PB5 MOSI on SPI3 -- where the UYUP-RPI-A
 * schematic routes it (nets PB3_SPI3_SCK and friends), all three on header
 * U5, and the same bus carrying the onboard W25Q32 flash with CS on PE3.
 *
 * This began on PC10/PC11/PC12, which is what the vendor's spi examples
 * use. Those are AF-correct and completely unreachable: on this board they
 * are the microSD socket. "Confirmed on the chip" and "reachable on the
 * board" are different claims, and conflating them cost a test round here
 * and two more on USART2's RX pin.
 *
 * GPIO_MUX_6 carries over from those examples but is NOT example-confirmed
 * for PB3/4/5. It also does not have to be believed: the flash answers 0x9F
 * with a JEDEC id, so EF 40 xx coming back proves the mapping outright.
 */
#include "host_api.h"
#include "registry.h"
#include "mdl_format.h"
#include "at32f435_437.h"
#include "console.h"
#include "FreeRTOS.h"
#include "task.h"

/* One byte at the slowest divider is ~57us at 144MHz/512; this bound only
 * trips if the peripheral is not clocking at all. */
#define SPI_POLL_LIMIT 2000000u

#define SPI_MAX_LEN    256u
#define SPI_DEFAULT_HZ 1000000u

/* SPI3 hangs off APB1. The divider is applied to this, so it is the number
 * every achievable bus speed is derived from -- and, like I2C's timing
 * word, it is checked rather than assumed. */
#define SPI_PCLK_HZ    144000000u

typedef struct {
    uint8_t     instance;
    spi_type   *periph;
    crm_periph_clock_type clock;
    gpio_type  *port;
    uint16_t    sck_pin, miso_pin, mosi_pin;
    gpio_pins_source_type sck_src, miso_src, mosi_src;
    uint8_t     mux;
    crm_periph_clock_type gpio_clock;
    bool        open;
    uint8_t     mode;       /* 0..3, the usual CPOL/CPHA pairing */
    uint32_t    hz;         /* what was actually achieved, not requested */
} spi_bus_t;

static spi_bus_t g_spis[] = {
    { 3, SPI3, CRM_SPI3_PERIPH_CLOCK, GPIOB,
      GPIO_PINS_3, GPIO_PINS_4, GPIO_PINS_5,
      GPIO_PINS_SOURCE3, GPIO_PINS_SOURCE4, GPIO_PINS_SOURCE5,
      GPIO_MUX_6, CRM_GPIOB_PERIPH_CLOCK,
      false, 0, SPI_DEFAULT_HZ },
};
#define SPI_COUNT (sizeof(g_spis) / sizeof(g_spis[0]))

static spi_bus_t *spi_lookup(int instance)
{
    for (unsigned i = 0; i < SPI_COUNT; i++) {
        if (g_spis[i].instance == (uint8_t)instance) {
            return &g_spis[i];
        }
    }
    return NULL;
}

/*
 * Pick the fastest divider that does not exceed the requested speed.
 *
 * Rounding DOWN rather than to nearest is the safe direction: a device
 * rated for 1MHz clocked at 1.125MHz may work, may work only warm, or may
 * corrupt one transfer in a thousand -- which is the sort of fault that
 * gets blamed on wiring for a week. Going slower than asked never breaks a
 * device.
 */
static spi_mclk_freq_div_type div_for(uint32_t want_hz, uint32_t *got_hz)
{
    static const struct { spi_mclk_freq_div_type sel; uint16_t div; } table[] = {
        { SPI_MCLK_DIV_2,   2   }, { SPI_MCLK_DIV_3,   3   },
        { SPI_MCLK_DIV_4,   4   }, { SPI_MCLK_DIV_8,   8   },
        { SPI_MCLK_DIV_16,  16  }, { SPI_MCLK_DIV_32,  32  },
        { SPI_MCLK_DIV_64,  64  }, { SPI_MCLK_DIV_128, 128 },
        { SPI_MCLK_DIV_256, 256 }, { SPI_MCLK_DIV_512, 512 },
    };
    const unsigned n = sizeof(table) / sizeof(table[0]);

    for (unsigned i = 0; i < n; i++) {
        uint32_t hz = SPI_PCLK_HZ / table[i].div;
        if (hz <= want_hz) {
            *got_hz = hz;
            return table[i].sel;
        }
    }
    /* Slower than the slowest divider can go: give the slowest rather than
     * silently running far faster than asked. */
    *got_hz = SPI_PCLK_HZ / table[n - 1].div;
    return table[n - 1].sel;
}

static void spi_apply(spi_bus_t *b)
{
    spi_init_type cfg;
    spi_default_para_init(&cfg);
    cfg.transmission_mode      = SPI_TRANSMIT_FULL_DUPLEX;
    cfg.master_slave_mode      = SPI_MODE_MASTER;
    cfg.first_bit_transmission = SPI_FIRST_BIT_MSB;
    cfg.frame_bit_num          = SPI_FRAME_8BIT;
    /* Software CS, and the internal level driven high: with hardware CS
     * disabled the peripheral watches its own NSS input, and a low there
     * makes a master think it lost arbitration and drop out of master
     * mode. Nothing on the pins would show why. */
    cfg.cs_mode_selection      = SPI_CS_SOFTWARE_MODE;

    /* The four standard modes, spelled out rather than computed, because
     * the CPOL/CPHA-to-mode-number mapping is exactly the kind of thing
     * that gets inverted once and then debugged for hours. */
    switch (b->mode) {
    case 1:
        cfg.clock_polarity = SPI_CLOCK_POLARITY_LOW;
        cfg.clock_phase    = SPI_CLOCK_PHASE_2EDGE;
        break;
    case 2:
        cfg.clock_polarity = SPI_CLOCK_POLARITY_HIGH;
        cfg.clock_phase    = SPI_CLOCK_PHASE_1EDGE;
        break;
    case 3:
        cfg.clock_polarity = SPI_CLOCK_POLARITY_HIGH;
        cfg.clock_phase    = SPI_CLOCK_PHASE_2EDGE;
        break;
    default:
        cfg.clock_polarity = SPI_CLOCK_POLARITY_LOW;
        cfg.clock_phase    = SPI_CLOCK_PHASE_1EDGE;
        break;
    }

    uint32_t got = 0;
    cfg.mclk_freq_division = div_for(b->hz, &got);
    b->hz = got;

    spi_enable(b->periph, FALSE);
    spi_init(b->periph, &cfg);
    /* HIGHT is the vendor header's spelling, not a typo here. */
    spi_software_cs_internal_level_set(b->periph, SPI_SWCS_INTERNAL_LEVEL_HIGHT);
    spi_enable(b->periph, TRUE);
}

bool host_spi_claim(int instance)
{
    spi_bus_t *b = spi_lookup(instance);
    if (b == NULL) {
        return false;
    }
    if (b->open) {
        return true;
    }

    /* Same reasoning as I2C's timing word: every achievable bus speed is
     * derived from this, so being wrong about it means every reported Hz
     * is wrong too -- quietly. */
    crm_clocks_freq_type clocks;
    crm_clocks_freq_get(&clocks);
    if (clocks.apb1_freq != SPI_PCLK_HZ) {
        mdl_console_puts("[host] spi: APB1 is ");
        console_put_u32(clocks.apb1_freq);
        mdl_console_puts(" Hz, but the divider table assumes ");
        console_put_u32(SPI_PCLK_HZ);
        mdl_console_puts(" Hz -- refusing rather than reporting wrong speeds\r\n");
        return false;
    }

    crm_periph_clock_enable(b->gpio_clock, TRUE);
    crm_periph_clock_enable(b->clock, TRUE);

    gpio_init_type gpio_init_struct;
    gpio_default_para_init(&gpio_init_struct);
    gpio_init_struct.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
    gpio_init_struct.gpio_out_type       = GPIO_OUTPUT_PUSH_PULL;
    gpio_init_struct.gpio_mode           = GPIO_MODE_MUX;
    gpio_init_struct.gpio_pull           = GPIO_PULL_NONE;
    gpio_init_struct.gpio_pins           = b->sck_pin | b->mosi_pin;
    gpio_init(b->port, &gpio_init_struct);

    /* MISO pulled up: an unconnected input pin otherwise floats, and a
     * floating MISO returns plausible-looking garbage instead of the
     * steady 0xFF that says "nothing is driving this". */
    gpio_init_struct.gpio_pull = GPIO_PULL_UP;
    gpio_init_struct.gpio_pins = b->miso_pin;
    gpio_init(b->port, &gpio_init_struct);

    gpio_pin_mux_config(b->port, b->sck_src,  b->mux);
    gpio_pin_mux_config(b->port, b->miso_src, b->mux);
    gpio_pin_mux_config(b->port, b->mosi_src, b->mux);

    b->mode = 0;
    b->hz   = SPI_DEFAULT_HZ;
    spi_apply(b);
    b->open = true;
    return true;
}

void host_spi_release(int instance)
{
    spi_bus_t *b = spi_lookup(instance);
    if (b == NULL || !b->open) {
        return;
    }
    spi_enable(b->periph, FALSE);
    spi_i2s_reset(b->periph);
    crm_periph_clock_enable(b->clock, FALSE);

    /* R1: put the pins back to a state the next owner can reason about.
     * SCK and MOSI are outputs here, and leaving them driven would keep
     * clocking or holding a line on a bus whose next owner is unknown. */
    gpio_init_type gpio_init_struct;
    gpio_default_para_init(&gpio_init_struct);
    gpio_init_struct.gpio_mode = GPIO_MODE_INPUT;
    gpio_init_struct.gpio_pull = GPIO_PULL_NONE;
    gpio_init_struct.gpio_pins = b->sck_pin | b->miso_pin | b->mosi_pin;
    gpio_init(b->port, &gpio_init_struct);

    b->open = false;
}

/* For the `spi` console command. */
int host_spi_describe(int index, int *instance, uint32_t *hz, int *mode)
{
    if (index < 0 || (unsigned)index >= SPI_COUNT) {
        return -1;
    }
    *instance = g_spis[index].instance;
    *hz       = g_spis[index].hz;
    *mode     = g_spis[index].mode;
    return (int)SPI_COUNT;
}

/* ---- the module-facing half ------------------------------------------ */

static spi_bus_t *checked_spi(int instance)
{
    if (g_mdl_slot.state == MDL_SLOT_EMPTY) {
        return NULL;
    }
    bool declared = false;
    for (uint8_t i = 0; i < g_mdl_slot.res_count; i++) {
        if (g_mdl_slot.res[i].kind == (uint8_t)MDL_RES_KIND_SPI &&
            g_mdl_slot.res[i].id == (uint8_t)instance) {
            declared = true;
            break;
        }
    }
    if (!declared) {
        return NULL;
    }
    spi_bus_t *b = spi_lookup(instance);
    return (b != NULL && b->open) ? b : NULL;
}

int host_spi_config_impl(int bus, int mode, uint32_t hz)
{
    spi_bus_t *b = checked_spi(bus);
    if (b == NULL) {
        return -1;
    }
    if (mode < 0 || mode > 3 || hz == 0u) {
        return -1;
    }
    b->mode = (uint8_t)mode;
    b->hz   = hz;
    spi_apply(b);
    /* The achieved speed, not the requested one. "You asked for 1MHz and
     * got 1.125MHz" is something a caller may need to know, and silently
     * rounding is how a marginal device becomes an intermittent one. */
    return (int)b->hz;
}

/*
 * One full-duplex transfer.
 *
 * tx may be NULL (send 0x00 and receive), rx may be NULL (send and discard)
 * -- both are common enough that forcing a dummy buffer on the caller would
 * just make every call site allocate one. Only non-NULL pointers are
 * validated, which keeps the containment check exactly as strong.
 */
int host_spi_transfer_impl(int bus, const void *tx, void *rx, uint32_t len)
{
    spi_bus_t *b = checked_spi(bus);
    if (b == NULL) {
        return -1;
    }
    if (len == 0u || len > SPI_MAX_LEN) {
        return -1;
    }
    if (tx == NULL && rx == NULL) {
        return -1;   /* a transfer that neither sends nor receives is a bug */
    }
    if (tx != NULL && !host_ptr_owned_by_module(tx, len)) {
        return -1;
    }
    if (rx != NULL && !host_ptr_owned_by_module(rx, len)) {
        return -1;
    }

    const uint8_t *out = (const uint8_t *)tx;
    uint8_t       *in  = (uint8_t *)rx;

    for (uint32_t i = 0; i < len; i++) {
        uint32_t spin = SPI_POLL_LIMIT;
        while (spi_i2s_flag_get(b->periph, SPI_I2S_TDBE_FLAG) == RESET) {
            if (--spin == 0u) {
                return -2;
            }
        }
        spi_i2s_data_transmit(b->periph, (out != NULL) ? out[i] : 0x00u);

        /* Wait for the byte that shifted IN while that one shifted out.
         * Doing this per byte rather than filling the FIFO first is what
         * keeps tx[i] and rx[i] paired, which is the whole contract of a
         * full-duplex transfer. */
        spin = SPI_POLL_LIMIT;
        while (spi_i2s_flag_get(b->periph, SPI_I2S_RDBF_FLAG) == RESET) {
            if (--spin == 0u) {
                return -2;
            }
        }
        uint8_t got = (uint8_t)spi_i2s_data_receive(b->periph);
        if (in != NULL) {
            in[i] = got;
        }
    }

    /* Let the last bit finish before returning, so an MDL that raises CS
     * immediately after does not cut the final byte in half. */
    uint32_t spin = SPI_POLL_LIMIT;
    while (spi_i2s_flag_get(b->periph, SPI_I2S_BF_FLAG) == SET) {
        if (--spin == 0u) {
            return -2;
        }
    }
    return 0;
}
