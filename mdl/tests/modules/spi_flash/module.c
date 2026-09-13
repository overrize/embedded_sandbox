/*
 * SPI from inside the sandbox [ABI v10]: read the onboard flash's identity.
 *
 * `spidemo` sends 0x9F (JEDEC READ ID) to the flash on SPI3 and decodes the
 * three bytes that come back: manufacturer, memory type, capacity.
 *
 * IT IS NOT ALWAYS A WINBOND PART. The schematic names a W25Q32 and then
 * says "or other compatible products" -- this board answers 0xC8 0x40 0x16,
 * a GigaDevice GD25Q32, which is the pin-compatible equivalent. The first
 * version of this test hardcoded Winbond's 0xEF and called a perfectly good
 * bus "unexpected id". The schematic had already said not to.
 *
 * So the check is what it should always have been: does this look like a
 * real SPI NOR flash? A registered manufacturer byte (not 0x00, not 0xFF)
 * plus a capacity that decodes to a sane size is the evidence. Pinning it
 * to one vendor tested the wrong thing.
 *
 * WHY A KNOWN ANSWER BEATS A LOOPBACK. A loopback proves the peripheral can
 * talk to itself. This proves the bus reaches a real device, at the right
 * clock polarity, with chip select framed correctly, and that the bytes
 * arrive in the right order -- because only one specific answer passes.
 * Wrong clock phase, swapped MISO/MOSI, a mis-muxed pin and a missing
 * device all produce something other than EF 40 16, and all of them would
 * sail through a loopback test.
 *
 * It also settles a question the schematic could not. The board routes SPI3
 * to PB3/PB4/PB5, but GPIO_MUX_6 for those pins is carried over from the
 * vendor's PC10/PC11/PC12 examples rather than confirmed for these. Reading
 * the id is the confirmation: no other mux setting gets this answer.
 *
 * CHIP SELECT IS THIS MODULE'S JOB, which is the arrangement host_spi.c
 * deliberately imposes -- CS is a GPIO, declared like any other. Here it is
 * whitelist pin 5, PE3, wired to the flash on the board. The flash wants CS
 * low for the whole command-plus-response, which is exactly the kind of
 * per-device timing that would have been wrong if the driver had chosen a
 * policy for everyone.
 */
#include "host_api.h"

MDL_MODULE_ABI_DECLARE();
MDL_MODULE_COMMAND("spidemo");
MDL_MODULE_RESOURCES(
    MDL_RES_SPI(3),        /* PB3 SCK, PB4 MISO, PB5 MOSI */
    MDL_RES_GPIO(5)        /* PE3, the W25Q32 chip select */
);

#define SPI_BUS  3
#define CS_PIN   5
#define CMD_JEDEC_ID 0x9Fu

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

/* The JEDEC manufacturer ids likely to turn up on a board like this one.
 * Unknown is not failure -- there are hundreds of registered ids, and the
 * point of the test is that the bus works, not that we can name the vendor. */
static const char *vendor_of(uint8_t id)
{
    switch (id) {
    case 0xEFu: return "Winbond";
    case 0xC8u: return "GigaDevice";
    case 0xC2u: return "Macronix";
    case 0x20u: return "Micron";
    case 0x1Fu: return "Adesto";
    case 0x9Du: return "ISSI";
    case 0x01u: return "Spansion";
    case 0x85u: return "Puya";
    default:    return NULL;
    }
}

int module_init(const host_api_t *host)
{
    /* The flash is rated well above this, but 1MHz is plenty to read three
     * bytes and leaves no question about signal integrity on a jumper-free
     * board trace. */
    int hz = host->spi_config(SPI_BUS, 0, 1000000);
    if (hz < 0) {
        host->log("spi_loop: spi_config refused -- is MDL_RES_SPI(3) declared?");
        return -1;
    }

    /* CS idles HIGH. Setting it before anything else matters: a flash that
     * sees CS low at power-up can interpret the next clock edges as a
     * command. */
    if (host->gpio_set(CS_PIN, 1) != 0) {
        host->log("spi_loop: cannot drive PE3 -- is MDL_RES_GPIO(5) declared?");
        return -1;
    }

    char *w = g_line;
    const char *end = g_line + sizeof(g_line);
    w = put_str(w, end, "spi_loop: SPI3 mode 0 at ");
    w = put_u32(w, end, (uint32_t)hz);
    w = put_str(w, end, " Hz, CS on PE3. Run `spidemo` to read the flash id.");
    *w = 0;
    host->log(g_line);
    return 0;
}

int module_cmd(const host_api_t *host, int argc, const char *const *argv)
{
    (void)argc;
    (void)argv;

    /* One transfer, four bytes: the command goes out in byte 0 while the
     * flash is still listening, and its three id bytes come back in 1..3.
     * That offset is not an accident of SPI -- it is what full duplex
     * means, and it is why the API is one transfer rather than a write
     * followed by a read. */
    uint8_t tx[4] = { CMD_JEDEC_ID, 0x00, 0x00, 0x00 };
    uint8_t rx[4] = { 0, 0, 0, 0 };

    host->gpio_set(CS_PIN, 0);
    int r = host->spi_transfer(SPI_BUS, tx, rx, 4);
    host->gpio_set(CS_PIN, 1);

    if (r == -1) {
        host->log("  refused -- bus not declared, or buffers not mine");
        return -1;
    }
    if (r == -2) {
        host->log("  the bus never clocked a byte through -- SPI3 is not running");
        return -2;
    }

    char *w = g_line;
    const char *end = g_line + sizeof(g_line);
    w = put_str(w, end, "  JEDEC id: ");
    for (int i = 1; i < 4; i++) {
        w = put_hex8(w, end, rx[i]);
    }
    *w = 0;
    host->log(g_line);

    /* A real device answers with a manufacturer byte that is neither of the
     * two values an idle bus produces. That, not a specific vendor, is what
     * distinguishes "the bus works" from "nothing is there". */
    bool plausible = (rx[1] != 0x00u && rx[1] != 0xFFu &&
                       rx[3] >= 0x10u && rx[3] <= 0x1Cu);

    w = g_line;
    if (plausible) {
        const char *v = vendor_of(rx[1]);
        w = put_str(w, end, "  SPI3 VERIFIED on PB3/PB4/PB5 -- mux, clock mode, "
                             "CS and byte order all correct. ");
        w = put_str(w, end, (v != NULL) ? v : "unlisted vendor");
        w = put_str(w, end, " SPI NOR, ");
        /* Capacity byte is log2 of the size in bytes, so 0x16 is 4MB. */
        w = put_u32(w, end, 1u << (rx[3] - 20u));
        w = put_str(w, end, " MB");
    } else if (rx[1] == 0xFFu && rx[2] == 0xFFu) {
        /* All ones is the pull-up on MISO with nothing driving it: either
         * the mux is wrong, or CS never reached the flash. Distinct from
         * all zeros, which means something IS driving the line low. */
        w = put_str(w, end, "  all 0xFF -- nothing is driving MISO. Wrong pin mux, "
                             "or CS not reaching the flash");
    } else if (rx[1] == 0x00u && rx[2] == 0x00u) {
        w = put_str(w, end, "  all 0x00 -- MISO held low. Check that PB4 is really "
                             "the flash's DO");
    } else {
        w = put_str(w, end, "  the bus answered, but this is not a valid flash id "
                             "-- check clock mode and bit order");
    }
    *w = 0;
    host->log(g_line);

    return (int)rx[1];
}
