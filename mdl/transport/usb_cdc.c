#include "usb_cdc.h"
#include "protocol.h"
#include "at32f435_437.h"
#include "usb_conf.h"
#include "usb_core.h"
#include "usbd_int.h"
#include "cdc_class.h"
#include "cdc_desc.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

/*
 * Supplied by the platform, not by this directory: the 48MHz OTGFS clock
 * is derived from whatever sclk the board's crystal and PLL produce, and
 * that is a board fact (mdl/tests/hil/common/board_clock.c for the
 * UYUP-RPI-A-2.4). Declared extern here rather than #include-ing that
 * header so mdl/transport stays independent of mdl/tests -- a second
 * board would provide its own definition and change nothing in this
 * file.
 */
extern void board_usb_clock_init(void);

/*
 * Serialises mdl_transport_write(). There are two writers now: the
 * supervisor task (command responses, console output) and the USB RX
 * task (per-character console echo, see mdl/transport/console.c).
 * Without this they can interleave inside a single usb_vcp_send_data()
 * IN transfer and produce spliced output. Created in usb_cdc_init(),
 * i.e. before the scheduler starts and therefore before either writer
 * exists.
 */
static SemaphoreHandle_t s_tx_lock;

/* otg_core_struct.dev is what usb_vcp_get_rxdata()/usb_vcp_send_data()
 * actually take -- matches the vendor virtual_comport example exactly
 * (project/at_start_f435/examples/usb_device/virtual_comport/src/main.c). */
otg_core_type otg_core_struct;

/* The vendor USB driver calls these two internally (declared, not
 * static, in its own sources) -- application code is expected to
 * supply them. Plain calibrated busy-wait: fine for USB's own internal
 * timing needs, and safe to call from anywhere (including before the
 * scheduler starts, during usb_cdc_init()), unlike vTaskDelay(). */
void usb_delay_us(uint32_t us)
{
    /* ~1 SystemCoreClock cycle per loop iteration is wildly pessimistic
     * for a tuned busy-wait, but this only needs to be a lower bound,
     * never exact -- USB's own protocol timing tolerances are wide
     * compared to what a few extra microseconds here costs. */
    volatile uint32_t count = (system_core_clock / 1000000u) * us / 4u;
    while (count--) {
        __NOP();
    }
}

void usb_delay_ms(uint32_t ms)
{
    while (ms--) {
        usb_delay_us(1000);
    }
}

void OTG_IRQ_HANDLER(void)
{
    usbd_irq_handler(&otg_core_struct);
}

/* ---- link-state feedback (see usb_cdc.h) ---------------------------- */

static volatile bool s_host_opened_port;

/*
 * Vendor callback, reached from cdc_class.c's SET_LINE_CODING handling
 * because inc/usb_conf.h defines USB_VIRTUAL_COMPORT. Its intended use
 * is to retune a real UART to the host's requested baud rate; there is
 * no real UART behind this CDC, so the line coding is discarded and the
 * call is used purely as the "a terminal just opened the port" edge.
 *
 * RUNS IN THE OTG INTERRUPT. Set a flag and nothing else -- in
 * particular do not write to the transport from here.
 */
void usb_usart_config(linecoding_type linecoding);
void usb_usart_config(linecoding_type linecoding)
{
    (void)linecoding;
    s_host_opened_port = true;
}

bool usb_cdc_link_up(void)
{
    return usbd_connect_state_get(&otg_core_struct.dev) == USB_CONN_STATE_CONFIGURED;
}

bool usb_cdc_take_host_open_event(void)
{
    if (!s_host_opened_port) {
        return false;
    }
    s_host_opened_port = false;
    return true;
}

static void usb_gpio_config(void)
{
    gpio_init_type gpio_init_struct;

    crm_periph_clock_enable(OTG_PIN_GPIO_CLOCK, TRUE);
    gpio_default_para_init(&gpio_init_struct);

    gpio_init_struct.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
    gpio_init_struct.gpio_out_type = GPIO_OUTPUT_PUSH_PULL;
    gpio_init_struct.gpio_mode = GPIO_MODE_MUX;
    gpio_init_struct.gpio_pull = GPIO_PULL_NONE;

    gpio_init_struct.gpio_pins = OTG_PIN_DP | OTG_PIN_DM;
    gpio_init(OTG_PIN_GPIO, &gpio_init_struct);
    gpio_pin_mux_config(OTG_PIN_GPIO, OTG_PIN_DP_SOURCE, OTG_PIN_MUX);
    gpio_pin_mux_config(OTG_PIN_GPIO, OTG_PIN_DM_SOURCE, OTG_PIN_MUX);

    /* No VBUS pin init -- inc/usb_conf.h defines USB_VBUS_IGNORE
     * (matches the vendor example's own default), so the USB core skips
     * VBUS sensing entirely. Add OTG_PIN_VBUS init here (and undefine
     * USB_VBUS_IGNORE) if your board needs real self/bus-powered
     * detection. */
}

void usb_cdc_init(void)
{
    nvic_priority_group_config(NVIC_PRIORITY_GROUP_4);

    usb_gpio_config();

    crm_periph_clock_enable(OTG_CLOCK, TRUE);

    /*
     * 48MHz for OTGFS, divided down from the 288MHz PLL --
     * board_usb_clock_init() (mdl/tests/hil/common/board_clock.c).
     *
     * This replaced a HICK + ACC (crystal-less, SOF-trimmed) setup that
     * was written when the board's hardware was unknown and the task
     * notes said a crystal might not exist. The UYUP-RPI-A-2.4 schematic
     * settles it: X2 is a real 24MHz crystal wired to OSC_IN/OSC_OUT, so
     * the PLL-derived clock is both available and more accurate than
     * trimming the internal RC against host SOF packets.
     *
     * The ordering constraint is absolute: board_clock_init() must
     * already have run, or sclk is still the 48MHz HICK default, the /6
     * divider yields 8MHz, and USB never enumerates. That is exactly the
     * failure this call used to hide by not depending on sclk at all.
     */
    board_usb_clock_init();

    s_tx_lock = xSemaphoreCreateMutex();
    configASSERT(s_tx_lock != NULL);

    nvic_irq_enable(OTG_IRQ, 0, 0);

    usbd_init(&otg_core_struct, USB_FULL_SPEED_CORE_ID, USB_ID,
              &cdc_class_handler, &cdc_desc_handler);
}

void usb_cdc_rx_task(void *pvParameters)
{
    (void)pvParameters;

    static uint8_t rx_buf[64]; /* USBD_CDC_OUT_MAXPACKET_SIZE, per cdc_class.h */

    for (;;) {
        uint16_t n = usb_vcp_get_rxdata(&otg_core_struct.dev, rx_buf);
        for (uint16_t i = 0; i < n; i++) {
            mdl_proto_rx_byte(rx_buf[i]);
        }
        /* 10ms poll: comfortably faster than a human/agent notices as
         * latency, comfortably slower than busy-polling every tick for
         * no reason. Not a hard real-time requirement -- USB CDC over a
         * bulk pipe has no timing guarantee to match anyway. */
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void mdl_transport_write(const uint8_t *data, uint32_t len)
{
    if (len == 0) {
        return;
    }
    /* Before the scheduler runs (or if mutex creation failed) there is
     * only one possible caller, so proceed unlocked rather than block. */
    bool locked = (s_tx_lock != NULL) &&
                  (xSemaphoreTake(s_tx_lock, portMAX_DELAY) == pdTRUE);

    /* usb_vcp_send_data() can return ERROR if the previous IN transfer
     * hasn't completed yet (matches the vendor example's own retry-loop
     * usage) -- bounded retry rather than an unbounded one, so a
     * genuinely disconnected host can't wedge the supervisor task
     * forever inside a response send. */
    uint32_t timeout = 50000;
    while (timeout--) {
        if (usb_vcp_send_data(&otg_core_struct.dev, (uint8_t *)data, (uint16_t)len) == SUCCESS) {
            if (locked) {
                xSemaphoreGive(s_tx_lock);
            }
            return;
        }
    }
    if (locked) {
        xSemaphoreGive(s_tx_lock);
    }
    /* Give up silently -- there is no reliable-delivery contract for
     * this transport (matches the vendor driver's own best-effort
     * send), and the PC side is expected to time out and retry the
     * whole request (tools/watch.py's job) rather than this layer
     * inventing a retransmission protocol on top of USB's own. */
}
