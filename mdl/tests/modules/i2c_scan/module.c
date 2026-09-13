/*
 * Drives I2C1 from inside the sandbox [ABI v7].
 *
 * `i2cscan` walks the 7-bit address space and reports which addresses
 * answer. It doubles as a test of the driver, because 112 back-to-back
 * probes with at most one device present is a harsh way to run a bus:
 * almost every transfer NACKs, so any state left behind by a failed one
 * lands on the next.
 *
 * That is not hypothetical -- it is how host_i2c.c's bus_recover() was
 * found. Before it existed this scan reported a device at an address that
 * MOVED between runs (0x68, then 0x60, then 0x60), which is the signature
 * of leftover state rather than of hardware: a real device answers at one
 * address every time.
 *
 * On this board the answer should be the AT24C64 EEPROM (U8 in the
 * schematic) somewhere in 0x50..0x57, depending on its A0/A1/A2 pins.
 *
 * With a device attached, its address appears in the list, and `i2crd
 * <addr> <reg>` reads one register from it.
 *
 * Note what this MDL is NOT doing: touching a peripheral register. It has
 * no MPU region over any peripheral and could not if it tried. Every
 * transfer is a gated call the host performs on its behalf, after checking
 * that this MDL declared MDL_RES_I2C(1) and that the buffers are inside
 * its own memory.
 */
#include "host_api.h"

MDL_MODULE_ABI_DECLARE();
MDL_MODULE_COMMAND("i2cscan");
MDL_MODULE_RESOURCES(MDL_RES_I2C(1));   /* PB6 SCL, PB7 SDA */

static char g_line[80];
static int  g_found = 0;

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
    if (w < end - 4) {
        *w++ = '0';
        *w++ = 'x';
        *w++ = d[(v >> 4) & 0xF];
        *w++ = d[v & 0xF];
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
    host->log("i2c_scan: I2C1 claimed; type `i2cscan` at the console");
    return 0;
}

int module_cmd(const host_api_t *host, int argc, const char *const *argv)
{
    (void)argc;
    (void)argv;

    /* One byte, into the module's own memory -- the host refuses a buffer
     * anywhere else, which is the point of the check. */
    uint8_t probe = 0;
    g_found = 0;

    char *w = g_line;
    const char *end = g_line + sizeof(g_line);
    w = put_str(w, end, "scanning I2C1:");
    *w = 0;
    host->log(g_line);

    /* 0x08..0x77: the addresses actually usable by devices. The ones below
     * and above are reserved by the I2C specification, and probing them
     * would be noise rather than information. */
    int refused = 0, nack = 0, noresp = 0;
    for (int addr = 0x08; addr <= 0x77; addr++) {
        int r = host->i2c_read(1, addr, &probe, 1);
        if (r == -3) { nack++; }
        if (r == -2) { noresp++; }
        if (r == 0) {
            w = g_line;
            w = put_str(w, end, "  device at ");
            w = put_hex8(w, end, (uint8_t)addr);
            *w = 0;
            host->log(g_line);
            g_found++;
        } else if (r == -1) {
            /* Refused, not "no answer" -- the bus is not declared, or the
             * host has no driver. Worth reporting once rather than 112
             * times, and worth distinguishing from an empty bus, which is
             * a completely different situation. */
            refused++;
        }
    }

    /* WHICH KIND of silence, not just that it was silent.
     *
     * -3 is ACKFAIL: the bus clocked an address out and nobody pulled SDA
     * down. That means the peripheral, the mux and the pull-ups all work,
     * and there is simply no device at that address.
     *
     * -2 is a timeout or an early-step failure: the transfer never got far
     * enough to hear an answer, which points at the peripheral or the pin
     * routing rather than at the bus being empty.
     *
     * Collapsing both into "no device" is what made an empty bus and a
     * misrouted one look identical -- the same mistake that hid I2C1 being
     * on the wrong pins for three rounds. */
    w = g_line;
    w = put_str(w, end, "  no-answer breakdown: ");
    w = put_u32(w, end, (uint32_t)nack);
    w = put_str(w, end, " NACK (bus works, nobody home) / ");
    w = put_u32(w, end, (uint32_t)noresp);
    w = put_str(w, end, " timeout (peripheral or routing)");
    *w = 0;
    host->log(g_line);

    w = g_line;
    if (refused > 0) {
        w = put_str(w, end, "  REFUSED by the host -- is MDL_RES_I2C(1) declared, "
                             "and does this firmware have the driver?");
    } else {
        w = put_str(w, end, "  found ");
        w = put_u32(w, end, (uint32_t)g_found);
        w = put_str(w, end, " device(s) on PB8/PB9. The board carries an "
                             "AT24C64 EEPROM (U8) at 0x50..0x57.");
    }
    *w = 0;
    host->log(g_line);
    return g_found;
}
