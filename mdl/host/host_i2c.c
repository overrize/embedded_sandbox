/*
 * I2C for MDLs: the first peripheral an MDL can actually drive.
 *
 * R3 gave MDLs the ability to CLAIM a peripheral and have the claim
 * arbitrated against pins. This is the other half -- the vtable entries
 * that let a claim do something. Everything R3 built still applies: an MDL
 * that has not declared MDL_RES_I2C(n) cannot touch bus n, and the check
 * happens on every call, not only at load.
 *
 * WHY THE HOST OWNS THE PERIPHERAL. An MDL has no MPU region over any
 * peripheral register -- that is what makes the sandbox worth having -- so
 * it cannot drive I2C itself even if it wanted to. Every transfer is a
 * gated call into this file, which is also the only place that can enforce
 * "this bus belongs to you" and "that buffer is inside your own memory".
 *
 * BLOCKING, WITH A BOUND. The vendor's polling API is used rather than the
 * interrupt or DMA variants: a transfer runs in the MDL's own task, so
 * blocking there blocks only that MDL, and the supervisor and USB task are
 * higher priority and keep running. The timeout is not optional -- an I2C
 * device that never ACKs would otherwise hang the module task forever, and
 * "the sensor is unplugged" is the normal case in the field, not an
 * exceptional one.
 */
#include "host_api.h"
#include "registry.h"
#include "mdl_format.h"
#include "at32f435_437.h"
#include "i2c_application.h"
#include "FreeRTOS.h"
#include "task.h"
#include "console.h"
#include <string.h>

/*
 * Clock control word for 100kHz, from the vendor's own i2c examples.
 *
 * It encodes SCLDEL/SDADEL/SCLH/SCLL as tick counts against the I2C
 * peripheral's input clock, so it is only correct for the APB1 frequency
 * it was computed for. Copying it blind and being wrong does not produce a
 * clean failure: it produces a bus that mostly works and intermittently
 * does not, which is among the worst things to debug on a device that is
 * already in a customer's hands.
 *
 * So the frequency is checked at init instead of assumed. If the board's
 * clocking changes, this refuses to come up and says why, rather than
 * running a subtly wrong bus.
 */
#define I2C_CLKCTRL_100K   0x80504C4Eu
#define I2C_CLKCTRL_APB1_HZ 144000000u

/* Long enough for a slow device to stretch the clock through a page write,
 * short enough that a missing device is reported rather than waited on.
 * Milliseconds, per the vendor API. */
#define I2C_TIMEOUT_MS 100u

/* Biggest single transfer. Bounded because the buffer has to be validated
 * as lying inside the MDL's own memory, and an unbounded length would make
 * that check meaningless. */
#define I2C_MAX_LEN 256u

/* Only I2C1 is wired up on this board (PB6/PB7, see board_pins.def). The
 * table is indexed by instance number so adding I2C2 is a row, not a
 * restructure. */
static i2c_handle_type s_hi2c1;

typedef struct {
    uint8_t           instance;   /* 1 for I2C1 */
    i2c_type         *periph;
    i2c_handle_type  *handle;
    crm_periph_clock_type clock;
    gpio_type        *port;
    uint16_t          scl_pin, sda_pin;
    gpio_pins_source_type scl_source, sda_source;
    bool              open;
} i2c_bus_t;

static i2c_bus_t g_buses[] = {
    { 1, I2C1, &s_hi2c1, CRM_I2C1_PERIPH_CLOCK, GPIOB,
      GPIO_PINS_6, GPIO_PINS_7, GPIO_PINS_SOURCE6, GPIO_PINS_SOURCE7, false },
};
#define BUS_COUNT (sizeof(g_buses) / sizeof(g_buses[0]))

static i2c_bus_t *bus_lookup(int instance)
{
    for (unsigned i = 0; i < BUS_COUNT; i++) {
        if (g_buses[i].instance == (uint8_t)instance) {
            return &g_buses[i];
        }
    }
    return NULL;
}

/*
 * Strong override of the vendor library's __WEAK hook. i2c_config() calls
 * it, so pin muxing and peripheral clocking happen where the vendor
 * expects them to, rather than being duplicated before the call.
 */
void i2c_lowlevel_init(i2c_handle_type *hi2c)
{
    i2c_bus_t *b = NULL;
    for (unsigned i = 0; i < BUS_COUNT; i++) {
        if (g_buses[i].handle == hi2c) {
            b = &g_buses[i];
            break;
        }
    }
    if (b == NULL) {
        return;
    }

    crm_periph_clock_enable(b->clock, TRUE);
    crm_periph_clock_enable(CRM_GPIOB_PERIPH_CLOCK, TRUE);

    gpio_init_type gpio_init_struct;
    gpio_default_para_init(&gpio_init_struct);
    gpio_init_struct.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
    gpio_init_struct.gpio_out_type       = GPIO_OUTPUT_OPEN_DRAIN;
    gpio_init_struct.gpio_mode           = GPIO_MODE_MUX;
    gpio_init_struct.gpio_pull           = GPIO_PULL_UP;
    gpio_init_struct.gpio_pins           = b->scl_pin | b->sda_pin;
    gpio_init(b->port, &gpio_init_struct);

    /* GPIO_MUX_4 is I2C1 on port B for this part -- from the vendor's
     * i2c/communication_int example, not from memory. */
    gpio_pin_mux_config(b->port, b->scl_source, GPIO_MUX_4);
    gpio_pin_mux_config(b->port, b->sda_source, GPIO_MUX_4);

    i2c_init(hi2c->i2cx, 0x0F, I2C_CLKCTRL_100K);
    i2c_own_address1_set(hi2c->i2cx, I2C_ADDRESS_MODE_7BIT, 0x00);
}

bool host_i2c_claim(int instance)
{
    i2c_bus_t *b = bus_lookup(instance);
    if (b == NULL) {
        return false;
    }
    if (b->open) {
        return true;
    }

    /* The precondition behind I2C_CLKCTRL_100K. Checked rather than
     * trusted, because being wrong here yields a bus that works most of
     * the time. */
    crm_clocks_freq_type clocks;
    crm_clocks_freq_get(&clocks);
    if (clocks.apb1_freq != I2C_CLKCTRL_APB1_HZ) {
        /* Say WHICH frequency, not just that it was wrong. The first cut
         * of this returned false silently, and the result was a bus that
         * refused every transfer with no way to tell whether the cause
         * was the clock, a missing declaration, or an unlinked driver.
         * A refusal that does not say why is the failure mode this whole
         * project keeps having to design against. */
        mdl_console_puts("[host] i2c: APB1 is ");
        console_put_u32(clocks.apb1_freq);
        mdl_console_puts(" Hz, but the 100kHz timing word assumes ");
        console_put_u32(I2C_CLKCTRL_APB1_HZ);
        mdl_console_puts(" Hz -- refusing rather than running wrong timing\r\n");
        return false;
    }

    b->handle->i2cx = b->periph;
    i2c_config(b->handle);
    b->open = true;
    return true;
}

void host_i2c_release(int instance)
{
    i2c_bus_t *b = bus_lookup(instance);
    if (b == NULL || !b->open) {
        return;
    }
    /* Reset before disabling: a transfer interrupted by an unload could
     * otherwise leave the bus with SDA held low, which wedges every future
     * master on it -- including the next MDL's. */
    i2c_reset(b->periph);
    i2c_enable(b->periph, FALSE);
    crm_periph_clock_enable(b->clock, FALSE);
    b->open = false;
}

/* ---- the module-facing half ----------------------------------------- */

/*
 * Both halves of "may this MDL do this", in one place so neither can be
 * forgotten at a call site: the bus must be one the MDL declared, and it
 * must actually be open.
 */
static i2c_bus_t *checked_bus(int instance)
{
    if (g_mdl_slot.state == MDL_SLOT_EMPTY) {
        return NULL;
    }
    bool declared = false;
    for (uint8_t i = 0; i < g_mdl_slot.res_count; i++) {
        if (g_mdl_slot.res[i].kind == (uint8_t)MDL_RES_KIND_I2C &&
            g_mdl_slot.res[i].id == (uint8_t)instance) {
            declared = true;
            break;
        }
    }
    if (!declared) {
        return NULL;
    }
    i2c_bus_t *b = bus_lookup(instance);
    return (b != NULL && b->open) ? b : NULL;
}

static int status_to_ret(i2c_status_type st)
{
    /* One negative code per outcome the caller can act on differently:
     * -2 says "nothing answered at that address", which is a wiring or
     * address question, while -3 says the transfer started and went wrong,
     * which is not. Collapsing them into -1 would throw away the only
     * distinction that changes what you check next. */
    switch (st) {
    case I2C_OK:       return 0;
    case I2C_ERR_TIMEOUT:
    case I2C_ERR_STEP_1:
    case I2C_ERR_STEP_2:
    case I2C_ERR_STEP_3: return -2;   /* no response in time */
    default:            return -3;   /* bus error, arbitration lost, NACK */
    }
}

int host_i2c_write_impl(int bus, int addr7, const void *data, uint32_t len)
{
    i2c_bus_t *b = checked_bus(bus);
    if (b == NULL) {
        return -1;
    }
    if (len == 0u || len > I2C_MAX_LEN || addr7 < 0 || addr7 > 0x7F) {
        return -1;
    }
    if (!host_ptr_owned_by_module(data, len)) {
        return -1;
    }
    /* The vendor API takes a left-aligned address; MDLs pass the 7-bit
     * address everyone reads off a datasheet. */
    return status_to_ret(i2c_master_transmit(b->handle, (uint16_t)(addr7 << 1),
                                              (uint8_t *)data, (uint16_t)len,
                                              I2C_TIMEOUT_MS));
}

int host_i2c_read_impl(int bus, int addr7, void *data, uint32_t len)
{
    i2c_bus_t *b = checked_bus(bus);
    if (b == NULL) {
        return -1;
    }
    if (len == 0u || len > I2C_MAX_LEN || addr7 < 0 || addr7 > 0x7F) {
        return -1;
    }
    if (!host_ptr_owned_by_module(data, len)) {
        return -1;
    }
    return status_to_ret(i2c_master_receive(b->handle, (uint16_t)(addr7 << 1),
                                             (uint8_t *)data, (uint16_t)len,
                                             I2C_TIMEOUT_MS));
}

/*
 * Write then read without releasing the bus in between.
 *
 * This exists because it is what almost every I2C device actually needs --
 * "write the register number, then read the value" -- and doing it as two
 * separate calls inserts a STOP between them. Plenty of devices reset
 * their internal address pointer on STOP, so the two-call version reads
 * the wrong register in a way that looks like a wiring fault.
 */
int host_i2c_write_read_impl(int bus, int addr7, const void *tx, uint32_t txlen,
                              void *rx, uint32_t rxlen)
{
    i2c_bus_t *b = checked_bus(bus);
    if (b == NULL) {
        return -1;
    }
    if (txlen == 0u || txlen > I2C_MAX_LEN || rxlen == 0u || rxlen > I2C_MAX_LEN) {
        return -1;
    }
    if (addr7 < 0 || addr7 > 0x7F) {
        return -1;
    }
    if (!host_ptr_owned_by_module(tx, txlen) ||
        !host_ptr_owned_by_module(rx, rxlen)) {
        return -1;
    }

    i2c_status_type st = i2c_master_transmit(b->handle, (uint16_t)(addr7 << 1),
                                              (uint8_t *)tx, (uint16_t)txlen,
                                              I2C_TIMEOUT_MS);
    if (st != I2C_OK) {
        return status_to_ret(st);
    }
    return status_to_ret(i2c_master_receive(b->handle, (uint16_t)(addr7 << 1),
                                             (uint8_t *)rx, (uint16_t)rxlen,
                                             I2C_TIMEOUT_MS));
}
