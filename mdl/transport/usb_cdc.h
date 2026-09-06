#ifndef MDL_TRANSPORT_USB_CDC_H
#define MDL_TRANSPORT_USB_CDC_H

#include <stdbool.h>

/*
 * USB OTG FS CDC-ACM transport, on top of Artery's vendor USB device
 * stack (middlewares/usb_drivers + middlewares/usbd_class/cdc), device
 * mode. The 48MHz USB clock is divided from the board's PLL --
 * board_usb_clock_init(), declared in mdl/tests/hil/common/board_clock.h
 * and called from usb_cdc_init().
 *
 * The vendor CDC driver is POLLING-based, not callback-based (confirmed
 * by reading cdc_class.c and the vendor's own virtual_comport example:
 * usb_vcp_get_rxdata() is meant to be called periodically, there is no
 * app-registrable "bytes arrived" hook) -- usb_cdc_rx_task() is that
 * poll loop, running as its own FreeRTOS task at MDL_USB_TASK_PRIORITY.
 */

/* Brings up OTGFS1 + the CDC class (GPIO mux, clock, NVIC, usbd_init()).
 * Call once from main(), before vTaskStartScheduler(). */
void usb_cdc_init(void);

/* FreeRTOS task entry point (xTaskCreate(usb_cdc_rx_task, ...)) -- polls
 * usb_vcp_get_rxdata() and feeds every received byte to
 * mdl_proto_rx_byte() (mdl/transport/protocol.c). Never returns. */
void usb_cdc_rx_task(void *pvParameters) __attribute__((noreturn));

/*
 * ---- link-state feedback -------------------------------------------
 * Two questions a person standing at the board actually asks, neither of
 * which the transport answered before: "is USB up?" and "did my terminal
 * actually connect?"
 */

/*
 * True once the host has enumerated AND configured the device, i.e. the
 * CDC endpoints are live. This is USB link state only -- it says nothing
 * about whether anyone has opened the COM port.
 *
 * Safe from any task; just reads the vendor stack's connect state.
 */
bool usb_cdc_link_up(void);

/*
 * True exactly once per "a host opened the serial port", then clears.
 *
 * The signal is the CDC SET_LINE_CODING request, which every terminal
 * emulator sends when it opens the port (that is also how it hands over
 * the baud rate this firmware then ignores -- CDC's baud is virtual).
 * The vendor stack routes it to an application callback,
 * usb_usart_config(), but only when USB_VIRTUAL_COMPORT is defined --
 * see mdl/tests/hil/at32f435_m4/inc/usb_conf.h. That callback runs in
 * the OTG interrupt, so it only sets a flag; this function is how a task
 * consumes it.
 *
 * Why it matters: the boot banner is printed once, long before anyone
 * plugs in a terminal, so opening the port used to show a blank screen
 * until you pressed Enter. Now the banner is reprinted on connect.
 */
bool usb_cdc_take_host_open_event(void);

#endif /* MDL_TRANSPORT_USB_CDC_H */
