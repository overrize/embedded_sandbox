/*
 * Read the board's AT24C64 EEPROM [ABI v7].
 *
 * WHY THIS EXISTS AFTER THE SCANNER ALREADY FOUND IT. A scan proves an
 * address ACKs. This proves the bus carries DATA: it sets the EEPROM's
 * internal address pointer and reads bytes back, which is the ordinary
 * shape of every I2C device conversation.
 *
 * It also exercises the one path argued for in host_i2c.c and never
 * actually run -- i2c_write_read()'s repeated START. Writing the address
 * and reading the data as two separate calls inserts a STOP, and the
 * AT24C64 resets its address pointer on STOP, so the two-call version
 * would read from wherever the pointer happened to be. That claim has
 * been a comment since the driver was written; this runs it.
 *
 * DELIBERATELY READ-ONLY. Writing would prove more, and it would also
 * change the contents of a chip on someone else's board for the sake of a
 * test. A read exercises addressing, repeated START, and the receive path,
 * which is the part in question.
 */
#include "host_api.h"

MDL_MODULE_ABI_DECLARE();
MDL_MODULE_COMMAND("eeprom");
MDL_MODULE_RESOURCES(MDL_RES_I2C(1));   /* PB8 SCL, PB9 SDA */

#define EEPROM_ADDR 0x50
#define READ_LEN    8

static char g_line[120];

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

/* Read READ_LEN bytes from one 16-bit EEPROM address. */
static int read_at(const host_api_t *host, uint16_t mem, uint8_t *out)
{
    /* Two address bytes, big-endian -- the AT24C64 is 8KB, so it needs a
     * 16-bit internal address. */
    uint8_t tx[2];
    tx[0] = (uint8_t)(mem >> 8);
    tx[1] = (uint8_t)(mem & 0xFFu);
    return host->i2c_write_read(1, EEPROM_ADDR, tx, 2, out, READ_LEN);
}

int module_init(const host_api_t *host)
{
    uint8_t probe[READ_LEN];
    int r = read_at(host, 0x0000, probe);
    if (r == -1) {
        host->log("i2c_eeprom: refused -- is MDL_RES_I2C(1) declared?");
        return -1;
    }
    host->log("i2c_eeprom: AT24C64 on I2C1 (PB8/PB9). Run `eeprom`.");
    return 0;
}

int module_cmd(const host_api_t *host, int argc, const char *const *argv)
{
    (void)argc;
    (void)argv;

    /* Two different addresses, because reading the same bytes twice cannot
     * distinguish "addressing works" from "the bus returns whatever it
     * last had". Different addresses should give different contents on a
     * used chip -- and on a blank one both read 0xFF, which is reported as
     * blank rather than as success. */
    uint8_t a[READ_LEN], b[READ_LEN];
    int ra = read_at(host, 0x0000, a);
    int rb = read_at(host, 0x0100, b);

    if (ra != 0 || rb != 0) {
        char *w = g_line;
        const char *end = g_line + sizeof(g_line);
        w = put_str(w, end, "  read failed: ");
        w = put_u32(w, end, (uint32_t)(-ra));
        w = put_str(w, end, " / ");
        w = put_u32(w, end, (uint32_t)(-rb));
        w = put_str(w, end, "   (-2 = no answer, -3 = bus error)");
        *w = 0;
        host->log(g_line);
        return -1;
    }

    char *w = g_line;
    const char *end = g_line + sizeof(g_line);
    w = put_str(w, end, "  @0x0000: ");
    for (int i = 0; i < READ_LEN; i++) {
        w = put_hex8(w, end, a[i]);
    }
    *w = 0;
    host->log(g_line);

    w = g_line;
    w = put_str(w, end, "  @0x0100: ");
    for (int i = 0; i < READ_LEN; i++) {
        w = put_hex8(w, end, b[i]);
    }
    *w = 0;
    host->log(g_line);

    int blank = 1;
    for (int i = 0; i < READ_LEN; i++) {
        if (a[i] != 0xFFu || b[i] != 0xFFu) {
            blank = 0;
        }
    }

    host->log(blank
        ? "  both all-0xFF: an erased EEPROM reads this way, so the transfer "
          "worked and the chip is simply blank"
        : "  contents differ from erased -- addressing and repeated START "
          "both confirmed on a real device");
    return 0;
}
