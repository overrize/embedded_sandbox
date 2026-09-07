#ifndef MDL_TRANSPORT_PROTOCOL_H
#define MDL_TRANSPORT_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/*
 * Wire framing for the USB CDC channel. Deliberately trivial (a byte
 * stream, not USB-transfer-boundary-aware -- CDC already gives us a
 * reliable ordered byte stream, so there is no need for anything fancier
 * than length-prefixing + a checksum to detect a torn/corrupted frame):
 *
 *   [magic:4]['MDLC'][cmd:1][len:4 LE][payload:len bytes][crc32:4 LE]
 *
 * crc32 covers cmd+len+payload (everything between magic and crc32
 * itself) -- same mdl_crc32() core/crc32.c already has for the module
 * format, reused here rather than adding a second checksum routine.
 *
 * This header is transport-medium-agnostic on purpose: mdl_proto_rx_byte()
 * doesn't know or care that the bytes came from USB CDC -- a future
 * transport (say, a debug UART fallback) could feed the same parser.
 * mdl/transport/usb_cdc.c is the only file that knows about the actual
 * AT32 OTGFS/CDC driver API.
 *
 * CONTEXT NOTE: Artery's vendor CDC driver (middlewares/usbd_class/cdc)
 * is polling-based, not callback-based -- usb_vcp_get_rxdata() is meant
 * to be called periodically from ordinary code, not pushed from an ISR
 * (confirmed by reading cdc_class.c/the vendor's own virtual_comport
 * example: its main loop polls every iteration; there is no
 * app-registrable "bytes arrived" callback in the shipped driver). So
 * unlike mdl/core/supervisor.c's fault path, mdl_proto_rx_byte() here is
 * called from mdl/transport/usb_cdc.c's dedicated RX task
 * (MDL_USB_TASK_PRIORITY, see FreeRTOSConfig.h) -- ordinary task
 * context, not an ISR. That's why frame-ready wakes the supervisor via
 * the plain xTaskNotifyGive() (task-safe call), not the *FromISR variant
 * mdl_supervisor_wake_from_isr() wraps for the fault path.
 */

#define MDL_PROTO_MAGIC 0x434C444Du /* 'MDLC' */

typedef enum {
    MDL_CMD_LOAD   = 1, /* payload: a packer.py .mdl file, verbatim */
    MDL_CMD_UNLOAD = 2, /* no payload */
    MDL_CMD_STATUS = 3, /* no payload */

    /* Load AND write to flash, so the MDL comes back after power loss.
     * A separate command rather than a flag on LOAD because the two have
     * genuinely different costs: an ordinary push is free and repeatable,
     * this one erases a flash sector. The development loop should not pay
     * that on every save of a source file. */
    MDL_CMD_LOAD_PERSIST = 4,
} mdl_proto_cmd_t;

typedef enum {
    MDL_RESP_OK     = 0x81, /* payload: none, or a short human string */
    MDL_RESP_ERROR  = 0x82, /* payload: a human-readable NUL-terminated
                              * diagnostic string -- same spirit as
                              * packer.py's "✗ ..." messages: always
                              * actionable, never a bare error code */
    MDL_RESP_STATUS = 0x83, /* payload: mdl_proto_status_t */
} mdl_proto_resp_t;

typedef struct {
    uint8_t  state;       /* mdl_slot_state_t */
    uint8_t  _reserved[3];
    uint32_t fault_pc;
    uint32_t fault_text_offset; /* 0xFFFFFFFF if no fault on record */
} mdl_proto_status_t;

/* Maximum accepted payload for MDL_CMD_LOAD -- generous over the v1
 * arena's actual text(16K)+data(8K)+got+reloc_table budget so a
 * too-big module gets packer.py's own clear rejection at mdl_load()
 * time (MDL_LOAD_ERR_TEXT_TOO_BIG etc.) rather than an earlier, less
 * specific "frame too big for the RX buffer" failure. */
#define MDL_PROTO_MAX_PAYLOAD (32u * 1024u)

/* mdl/core/supervisor.c calls this once, alongside mdl_supervisor_init(),
 * passing its own TaskHandle_t (as a bare void* -- this header stays
 * FreeRTOS-type-free the same way registry.h's task_handle field does)
 * so protocol.c knows who to xTaskNotifyGive() on a completed frame. */
void mdl_proto_set_supervisor_handle(void *supervisor_task_handle);

/*
 * Called once per received byte, from mdl/transport/usb_cdc.c's RX task
 * (ordinary FreeRTOS task context -- see the file comment above for why
 * this isn't ISR context for this particular transport). Accumulates
 * bytes into an internal static frame buffer; when a complete,
 * checksum-valid frame lands, sets a pending-frame flag and wakes the
 * supervisor task (xTaskNotifyGive()) rather than acting on the frame
 * itself -- "加载/卸载操作本身在独立的 loader 任务里做,不能在中断里做"
 * applies here exactly as it does to fault recovery, even though
 * neither this function nor its caller happens to run in an ISR for
 * this transport.
 */
void mdl_proto_rx_byte(uint8_t b);

/*
 * Supervisor-task context only: call after being woken by a protocol
 * notification. Returns true if a complete frame was pending (and
 * consumes it -- the internal buffer is ready for the next frame after
 * this returns), false if the wake was spurious (e.g. coalesced with a
 * fault notification on the same wait). out_cmd/out_payload/out_len
 * describe the frame; out_payload points into the internal static
 * buffer and is valid only until the next mdl_proto_rx_byte() call
 * completes a further frame -- the caller (mdl/core/supervisor.c) must
 * finish acting on it (mdl_load(), etc.) before returning to its wait
 * loop, not stash the pointer for later.
 */
bool mdl_proto_take_frame(mdl_proto_cmd_t *out_cmd, const uint8_t **out_payload, uint32_t *out_len);

/*
 * Supervisor-task context only: builds and sends a response frame.
 * mdl_proto_send() is implemented by mdl/transport/usb_cdc.c (the only
 * file that knows how to actually write bytes out over CDC) -- this
 * header just declares the framing helper that calls it.
 */
void mdl_proto_send_response(mdl_proto_resp_t resp, const void *payload, uint32_t len);

/* Implemented by mdl/transport/usb_cdc.c: write len raw bytes out over
 * the CDC channel. May block briefly (e.g. waiting for a USB IN
 * endpoint to free up) but must not block indefinitely -- called only
 * from supervisor-task context, never from an ISR. */
void mdl_transport_write(const uint8_t *data, uint32_t len);

#endif /* MDL_TRANSPORT_PROTOCOL_H */
