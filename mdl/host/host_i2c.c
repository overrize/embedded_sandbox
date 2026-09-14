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

/*
 * A SPIN COUNT, not milliseconds.
 *
 * The vendor's i2c_wait_flag()/i2c_wait_end() use this parameter as a raw
 * `if ((timeout--) == 0)` loop counter. It carries no time unit at all, and
 * the vendor example passes 0xFFFFFFF.
 *
 * This was `I2C_TIMEOUT_SPINS 100u`, with a comment asserting milliseconds
 * 'per the vendor API'. At 288MHz, 100 iterations is well under a
 * microsecond, while one byte at 100kHz takes about 90us -- so EVERY
 * transfer aborted before the bus could do anything. The bus had been
 * configured correctly the whole time: a scan of 112 addresses returned
 * 112 timeouts and ZERO NACKs, which is the signature of a transfer that
 * never starts rather than of an empty bus.
 *
 * The lesson is narrower than 'read the docs'. I gave a unitless parameter
 * a unit by naming it, and from then on the name was the only evidence for
 * the claim -- including to me, rereading my own code.
 *
 * 2,000,000 is roughly 100ms at 288MHz: an order of magnitude, not a
 * guarantee, since the loop body is a couple of register reads. Each wait
 * inside one transfer gets the full count, so a worst case is a few times
 * this.
 */
#define I2C_TIMEOUT_SPINS 2000000u

/* Biggest single transfer. Bounded because the buffer has to be validated
 * as lying inside the MDL's own memory, and an unbounded length would make
 * that check meaningless. */
#define I2C_MAX_LEN 256u

/* Only I2C1 is wired up on this board (PB8/PB9, see board_pins.def). The
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
      GPIO_PINS_8, GPIO_PINS_9, GPIO_PINS_SOURCE8, GPIO_PINS_SOURCE9, false },
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

    /* GPIO_MUX_4 is I2C1 on port B, from the vendor's i2c example (which
     * uses PB6/PB7; the MUX is the same across the port's I2C1 pins).
     *
     * PB8/PB9 rather than PB6/PB7 because that is where THIS board routes
     * I2C1 -- and, decisively, where its 10k pull-ups are. PB6/PB7 here go
     * to the camera connector with no pull-ups, so the earlier version
     * drove a bus that could never have worked, and scanned clean anyway
     * because an empty bus and the wrong bus are indistinguishable with
     * nothing attached. */
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

/* Defined below; the shell half needs them and comes first so it sits
 * next to the claim/release it wraps. */
static void bus_recover(i2c_bus_t *b);
static int status_to_ret(i2c_status_type st);

/* ---- the host shell's half [W2] -------------------------------------- */

/*
 * Poking the bus from the console.
 *
 * WHY THIS IS NOT THE MODULE PATH. checked_bus() asks whether the
 * CALLING MODULE declared this bus, and the console is not a module --
 * it would be refused. But the deeper reason is the one the arbitration
 * exists for: if a module holds I2C1 and the console transfers on it
 * too, that is two masters interleaving on one wire. So these refuse a
 * bus any module has claimed, rather than sharing it.
 *
 * Claim and release around each transfer rather than leaving the bus
 * up: a shell command should not change what the next module finds.
 */
static bool shell_bus_free(int instance)
{
    for (int i = 0; i < MDL_MAX_SLOTS; i++) {
        const module_t *m = &g_mdl_slots[i];
        if (m->state == MDL_SLOT_EMPTY) {
            continue;
        }
        for (uint8_t r = 0; r < m->res_count; r++) {
            if (m->res[r].kind == (uint8_t)MDL_RES_KIND_I2C &&
                m->res[r].id == (uint8_t)instance) {
                return false;
            }
        }
    }
    return true;
}

int host_i2c_shell(int bus, int addr7, const uint8_t *tx, uint32_t txlen,
                    uint8_t *rx, uint32_t rxlen)
{
    if (!shell_bus_free(bus)) {
        return -4;   /* a module owns it; sharing would be two masters */
    }
    i2c_bus_t *b = bus_lookup(bus);
    if (b == NULL || addr7 < 0 || addr7 > 0x7F) {
        return -1;
    }
    if (txlen > I2C_MAX_LEN || rxlen > I2C_MAX_LEN ||
        (txlen == 0u && rxlen == 0u)) {
        return -1;
    }

    bool was_open = b->open;
    if (!was_open && !host_i2c_claim(bus)) {
        return -1;
    }

    i2c_status_type st = I2C_OK;
    if (txlen > 0u) {
        st = i2c_master_transmit(b->handle, (uint16_t)(addr7 << 1),
                                  (uint8_t *)tx, (uint16_t)txlen,
                                  I2C_TIMEOUT_SPINS);
        if (st != I2C_OK) {
            bus_recover(b);
        }
    }
    if (st == I2C_OK && rxlen > 0u) {
        st = i2c_master_receive(b->handle, (uint16_t)(addr7 << 1),
                                 rx, (uint16_t)rxlen, I2C_TIMEOUT_SPINS);
        if (st != I2C_OK) {
            bus_recover(b);
        }
    }

    if (!was_open) {
        host_i2c_release(bus);
    }
    return status_to_ret(st);
}

/* ---- the module-facing half ----------------------------------------- */

/*
 * Both halves of "may this MDL do this", in one place so neither can be
 * forgotten at a call site: the bus must be one the MDL declared, and it
 * must actually be open.
 */
static i2c_bus_t *checked_bus(int instance)
{
    /* Asks the CALLER's declarations, not "the" module's [S0]. */
    if (!mdl_caller_declared((uint8_t)MDL_RES_KIND_I2C, (uint8_t)instance)) {
        return NULL;
    }
    i2c_bus_t *b = bus_lookup(instance);
    return (b != NULL && b->open) ? b : NULL;
}

/*
 * Put the peripheral back to a known state after a failed transfer.
 *
 * WHY THIS IS NOT OPTIONAL. Found by scanning: sweeping 112 addresses with
 * nothing but one device on the bus produced a 'device found' at an address
 * that CHANGED between runs -- 0x68, then 0x60, then 0x60. A real device
 * answers at one address every time, so that was state from the 111 failed
 * probes bleeding into the next one and eventually being read as success.
 *
 * The vendor library does clear ACKFAIL and report the error, so the return
 * code was right each time; what it does not do is guarantee the bus is
 * idle afterwards. A scan is an unusually harsh way to expose that, but an
 * application hits the same thing more quietly: one NACK from an absent
 * sensor, and the NEXT transfer to a device that IS there returns garbage.
 * That is the kind of fault that gets called intermittent for a week.
 *
 * Only on the error path, so a healthy bus pays nothing.
 */
static void bus_recover(i2c_bus_t *b)
{
    i2c_reset(b->periph);
    i2c_config(b->handle);   /* reset clears everything, so reconfigure */
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
    i2c_status_type st = i2c_master_transmit(b->handle, (uint16_t)(addr7 << 1),
                                              (uint8_t *)data, (uint16_t)len,
                                              I2C_TIMEOUT_SPINS);
    if (st != I2C_OK) {
        bus_recover(b);
    }
    return status_to_ret(st);
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
    i2c_status_type st = i2c_master_receive(b->handle, (uint16_t)(addr7 << 1),
                                             (uint8_t *)data, (uint16_t)len,
                                             I2C_TIMEOUT_SPINS);
    if (st != I2C_OK) {
        bus_recover(b);
    }
    return status_to_ret(st);
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
                                              I2C_TIMEOUT_SPINS);
    if (st != I2C_OK) {
        bus_recover(b);
        return status_to_ret(st);
    }
    st = i2c_master_receive(b->handle, (uint16_t)(addr7 << 1),
                             (uint8_t *)rx, (uint16_t)rxlen, I2C_TIMEOUT_SPINS);
    if (st != I2C_OK) {
        bus_recover(b);
    }
    return status_to_ret(st);
}
