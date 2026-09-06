/*
 * Adapted from Artery's vendor example config at
 * project/at_start_f435/examples/usb_device/virtual_comport/inc/usb_conf.h
 * (AT32F435_437_Firmware_Library_V2.2.6) -- device mode only (host mode
 * stripped, we never need it), OTGFS1 selected. USB_VBUS_IGNORE is kept
 * defined (as the vendor's own file has it) so usb_cdc.c's GPIO config
 * does not need to wire up a VBUS sense pin -- matches the task's own
 * "USB OTG FS,CDC 类,device 模式无需外部晶振" note (this covers the
 * USB *clock* crystal question separately -- see usb_cdc_init()'s
 * usb_clock48m_select() call).
 */
#ifndef __USB_CONF_H
#define __USB_CONF_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h> /* usbd_sdr.c uses NULL without including this itself --
                      * a vendor code gap; supplying it here (picked up
                      * transitively via usb_conf.h's own include chain)
                      * is the least invasive fix that doesn't touch
                      * vendor source. */
#include "at32f435_437_usb.h"
#include "at32f435_437.h"

#define USE_OTG_DEVICE_MODE

/*
 * Routes the CDC SET_LINE_CODING request to the application callback
 * usb_usart_config(), which mdl/transport/usb_cdc.c implements. The
 * vendor's intent is "retune a real UART to the host's baud rate"; there
 * is no real UART here, so it is used instead as the one reliable
 * indication that a terminal has OPENED the port -- see usb_cdc.h's
 * usb_cdc_take_host_open_event().
 *
 * Grepped before enabling: this macro guards exactly one call site in
 * the whole vendor tree (middlewares/usbd_class/cdc/cdc_class.c), so it
 * turns on that callback and nothing else.
 */
#define USB_VIRTUAL_COMPORT

/*
 * WHICH OTG INSTANCE, i.e. WHICH PINS.
 *
 * The UYUP-RPI-A-2.4 routes the USB1 connector's D+/D- to the MCU
 * through one of two alternative 0-ohm jumper pairs, because the board
 * has to suit several different MCU families:
 *
 *   R69/R70 fitted -> LQFP100 pins 70/71 = PA11/PA12 -> OTGFS1 -> set 1
 *   R71/R73 fitted -> LQFP100 pins 53/54 = PB14/PB15 -> OTGFS2 -> set 2
 *
 * (That is what the schematic net names PB14_PA11 and PB15_PA12_USB_ON
 * mean -- each is one net with two possible destinations.)
 *
 * Only one pair is populated. If the host does not enumerate anything at
 * all -- not even an unknown device -- checking these two pairs with a
 * multimeter is the first thing to do, and flipping this single number
 * is the whole fix. Both instances exist on the AT32F435, so either
 * value is legitimate; only the board decides which is right.
 */
#define OTG_USB_ID 1

#if (OTG_USB_ID == 1)
#define USB_ID                0
#define OTG_CLOCK             CRM_OTGFS1_PERIPH_CLOCK
#define OTG_IRQ                OTGFS1_IRQn
#define OTG_IRQ_HANDLER         OTGFS1_IRQHandler
#define OTG_WKUP_IRQ            OTGFS1_WKUP_IRQn
#define OTG_WKUP_HANDLER        OTGFS1_WKUP_IRQHandler
#define OTG_WKUP_EXINT_LINE     EXINT_LINE_18

#define OTG_PIN_GPIO            GPIOA
#define OTG_PIN_GPIO_CLOCK      CRM_GPIOA_PERIPH_CLOCK

#define OTG_PIN_DP              GPIO_PINS_12
#define OTG_PIN_DP_SOURCE       GPIO_PINS_SOURCE12

#define OTG_PIN_DM              GPIO_PINS_11
#define OTG_PIN_DM_SOURCE       GPIO_PINS_SOURCE11

/*
 * DO NOT WIRE THESE TWO UP ON THIS BOARD.
 *
 * They are the vendor's OTG VBUS-sense and ID pins (PA9/PA10), kept here
 * only because the vendor's own usb_conf.h template defines them. On the
 * UYUP-RPI-A-2.4, PA9/PA10 are the USART1 pair (PA9_U1_TX / PA10_U1_RX)
 * bridged to the on-board DAP's USB-serial endpoint. Calling
 * gpio_pin_mux_config() on either -- which is what "enabling VBUS
 * sensing" would mean -- takes the debug UART away.
 *
 * That is not a limitation worth working around, because USB1 on this
 * board is a DEVICE-only port regardless: the USB-C connector routes
 * only D+/D- to the MCU (through U2, SRV05-4A). Its VBUS reaches no MCU
 * pin, there is no 5V supply switch to power a downstream device, and
 * USB-C has no ID pin at all (role is negotiated on CC, which is not
 * connected to the MCU either). USB_VBUS_IGNORE below is therefore the
 * correct and permanent setting, not a placeholder.
 */
#define OTG_PIN_VBUS            GPIO_PINS_9
#define OTG_PIN_VBUS_SOURCE     GPIO_PINS_SOURCE9

#define OTG_PIN_ID              GPIO_PINS_10
#define OTG_PIN_ID_SOURCE       GPIO_PINS_SOURCE10

#define OTG_PIN_MUX             GPIO_MUX_10
#endif

#if (OTG_USB_ID == 2)
/*
 * OTGFS2 on PB14 (DM) / PB15 (DP) -- the R71/R73 jumper option.
 * Values match the vendor's own usb_conf.h template
 * (project/at_start_f435/examples/usb_device/virtual_comport/inc/).
 * Note GPIO_MUX_12 here, not GPIO_MUX_10: the alternate-function number
 * differs between the two OTG instances, so this is not a copy of the
 * block above with the pin numbers swapped.
 *
 * PB14/PB15 carry no competing function on this board (unlike PA9/PA10
 * above), so there is no VBUS/ID caveat to repeat here -- USB1 is still
 * device-only for the connector-level reasons given above, whichever
 * instance drives it.
 */
#define USB_ID                1
#define OTG_CLOCK             CRM_OTGFS2_PERIPH_CLOCK
#define OTG_IRQ                 OTGFS2_IRQn
#define OTG_IRQ_HANDLER         OTGFS2_IRQHandler
#define OTG_WKUP_IRQ            OTGFS2_WKUP_IRQn
#define OTG_WKUP_HANDLER        OTGFS2_WKUP_IRQHandler
#define OTG_WKUP_EXINT_LINE     EXINT_LINE_20

#define OTG_PIN_GPIO            GPIOB
#define OTG_PIN_GPIO_CLOCK      CRM_GPIOB_PERIPH_CLOCK

#define OTG_PIN_DP              GPIO_PINS_15
#define OTG_PIN_DP_SOURCE       GPIO_PINS_SOURCE15

#define OTG_PIN_DM              GPIO_PINS_14
#define OTG_PIN_DM_SOURCE       GPIO_PINS_SOURCE14

#define OTG_PIN_VBUS            GPIO_PINS_13
#define OTG_PIN_VBUS_SOURCE     GPIO_PINS_SOURCE13

#define OTG_PIN_ID              GPIO_PINS_12
#define OTG_PIN_ID_SOURCE       GPIO_PINS_SOURCE12

#define OTG_PIN_MUX             GPIO_MUX_12
#endif

#ifdef USE_OTG_DEVICE_MODE
#define USBD_RX_SIZE       128
#define USBD_EP0_TX_SIZE   24
#define USBD_EP1_TX_SIZE   20
#define USBD_EP2_TX_SIZE   20
#define USBD_EP3_TX_SIZE   20
#define USBD_EP4_TX_SIZE   20
#define USBD_EP5_TX_SIZE   20
#define USBD_EP6_TX_SIZE   20
#define USBD_EP7_TX_SIZE   20

#ifndef USB_EPT_MAX_NUM
#define USB_EPT_MAX_NUM 8
#endif

/* usbd_core.c's usbd_fifo_alloc() references the OTG2 fifo macros
 * unconditionally (not guarded by OTG_USB_ID) even though this build
 * only uses OTG1 -- a vendor code quirk, not a v1-specific need for
 * OTG2. Defined here (matching the vendor example's own values)
 * purely so that dead code path compiles; usbd_init() is called with
 * USB_ID=0 (OTG1) elsewhere, so these values are never actually used
 * at runtime. */
#define USBD2_RX_SIZE     128
#define USBD2_EP0_TX_SIZE 24
#define USBD2_EP1_TX_SIZE 20
#define USBD2_EP2_TX_SIZE 20
#define USBD2_EP3_TX_SIZE 20
#define USBD2_EP4_TX_SIZE 20
#define USBD2_EP5_TX_SIZE 20
#define USBD2_EP6_TX_SIZE 20
#define USBD2_EP7_TX_SIZE 20
#endif

/* No VBUS sense pin wiring needed -- matches the vendor example's own
 * default. Remove this if your board needs real VBUS sensing (self-
 * powered vs. bus-powered detection). */
#define USB_VBUS_IGNORE

/* Deliberately NOT defining USB_VIRTUAL_COMPORT: that flag turns on the
 * vendor example's USB<->hardware-UART bridge behavior in cdc_class.c
 * (SET_LINE_CODING re-configures a real UART baud rate via an
 * app-supplied usb_usart_config()) -- this project uses CDC purely as a
 * raw byte pipe for mdl/transport/protocol.c's own framing, with no
 * UART bridge, so that function is never called and doesn't need to
 * exist. */

void usb_delay_ms(uint32_t ms);
void usb_delay_us(uint32_t us);

#ifdef __cplusplus
}
#endif

#endif /* __USB_CONF_H */
