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

/*
 * Derives the 48MHz USB clock from HICK (the internal RC oscillator)
 * with ACC auto-trim against the USB host's own SOF timing, instead of
 * dividing it down from HEXT -- per the task's own hardware note ("USB
 * OTG FS, CDC 类, device 模式无需外部晶振"), this board may have no
 * external crystal at all. Values (c1/c2/c3 trim window, ACC_SOF_OTG1)
 * match Artery's own vendor example
 * (project/at_start_f435/examples/usb_device/virtual_comport/src/main.c's
 * usb_clock48m_select(USB_CLK_HICK) branch) -- OTG1 since OTG_USB_ID==1
 * in inc/usb_conf.h.
 */
static void usb_clock48m_select_hick(void)
{
    crm_usb_clock_source_select(CRM_USB_CLOCK_SOURCE_HICK);

    crm_periph_clock_enable(CRM_ACC_PERIPH_CLOCK, TRUE);

    acc_write_c1(7980);
    acc_write_c2(8000);
    acc_write_c3(8020);
    acc_sof_select(ACC_SOF_OTG1);
    acc_calibration_mode_enable(ACC_CAL_HICKTRIM, TRUE);
}

void usb_cdc_init(void)
{
    nvic_priority_group_config(NVIC_PRIORITY_GROUP_4);

    usb_gpio_config();

    crm_periph_clock_enable(OTG_CLOCK, TRUE);
    usb_clock48m_select_hick();

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
    /* usb_vcp_send_data() can return ERROR if the previous IN transfer
     * hasn't completed yet (matches the vendor example's own retry-loop
     * usage) -- bounded retry rather than an unbounded one, so a
     * genuinely disconnected host can't wedge the supervisor task
     * forever inside a response send. */
    uint32_t timeout = 50000;
    while (timeout--) {
        if (usb_vcp_send_data(&otg_core_struct.dev, (uint8_t *)data, (uint16_t)len) == SUCCESS) {
            return;
        }
    }
    /* Give up silently -- there is no reliable-delivery contract for
     * this transport (matches the vendor driver's own best-effort
     * send), and the PC side is expected to time out and retry the
     * whole request (tools/watch.py's job) rather than this layer
     * inventing a retransmission protocol on top of USB's own. */
}
