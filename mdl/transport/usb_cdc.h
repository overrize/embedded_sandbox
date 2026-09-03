#ifndef MDL_TRANSPORT_USB_CDC_H
#define MDL_TRANSPORT_USB_CDC_H

/*
 * USB OTG FS CDC-ACM transport, on top of Artery's vendor USB device
 * stack (middlewares/usb_drivers + middlewares/usbd_class/cdc). Device
 * mode, no external crystal needed (per the task's own hardware note --
 * usb_clock48m_select(USB_CLK_HEXT) below does assume HEXT is present
 * for the 48MHz USB clock specifically; swap to USB_CLK_HICK + ACC trim
 * in usb_cdc_init() if your board has no HEXT at all).
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

#endif /* MDL_TRANSPORT_USB_CDC_H */
