#include "protocol.h"
#include "console.h"
#include "crc32.h"
#include "FreeRTOS.h"
#include "task.h"
#include <string.h>

/*
 * Byte-at-a-time frame parser. Static buffer sized for
 * MDL_PROTO_MAX_PAYLOAD -- generous over what a v1 arena-sized module
 * actually needs (see the header's comment), so a legitimately-sized
 * module never gets rejected here; only genuinely oversized/garbled
 * input does.
 */
typedef enum {
    RX_MAGIC,
    RX_CMD,
    RX_LEN,
    RX_PAYLOAD,
    RX_CRC,
} rx_state_t;

static rx_state_t s_state = RX_MAGIC;
static uint32_t   s_field_pos;
static uint8_t     s_magic_buf[4];
static uint8_t     s_cmd;
static uint8_t     s_len_buf[4];
static uint32_t    s_len;
static uint8_t     s_payload[MDL_PROTO_MAX_PAYLOAD];
static uint8_t     s_crc_buf[4];

static volatile bool     s_frame_ready;
static mdl_proto_cmd_t   s_ready_cmd;
static uint32_t          s_ready_len;

static TaskHandle_t s_supervisor_handle_for_proto;

/*
 * Weak no-op default for the transport's one output primitive.
 *
 * mdl/transport/usb_cdc.c provides the real, strong definition, and any
 * build that has a transport links it. But mdl/core/supervisor.c calls
 * into this file unconditionally (it has since M4 added protocol
 * handling to mdl_supervisor_run()), so every target that builds
 * supervisor.c now needs protocol.c to link -- including M3, which
 * predates the transport and has no CDC hardware wired up at all. On
 * such a target the parser simply never receives a byte and this stub is
 * never reached; defining it weakly here is what lets M3 link without
 * either an #ifdef in supervisor.c or a fake transport file in M3's own
 * source list. Same pattern as mdl_supervisor_wake_from_isr()'s weak
 * default in mdl/core/registry.c, for the same reason.
 */
__attribute__((weak)) void mdl_transport_write(const uint8_t *data, uint32_t len)
{
    (void)data;
    (void)len;
}

void mdl_proto_set_supervisor_handle(void *h)
{
    s_supervisor_handle_for_proto = (TaskHandle_t)h;
}

static void reset_parser(void)
{
    s_state = RX_MAGIC;
    s_field_pos = 0;
}

void mdl_proto_rx_byte(uint8_t b)
{
    switch (s_state) {
    case RX_MAGIC: {
        /*
         * Demultiplex binary frames from typed console text.
         *
         * Match the magic INCREMENTALLY rather than filling a 4-byte
         * window and testing it: a window would hold the three most
         * recent bytes hostage until a fourth arrived, so an
         * interactive typist would see the console run three characters
         * behind, and pressing Enter would do nothing until three more
         * keys were pressed. Matching byte by byte means a byte is only
         * held while it could still turn out to be part of the magic;
         * the moment the prefix breaks, everything held is released to
         * the console in order.
         *
         * The magic is "MDLC" (all capitals), so ordinary lowercase
         * commands are never held at all.
         */
        static const uint8_t magic_bytes[4] = {
            (uint8_t)(MDL_PROTO_MAGIC & 0xFFu),
            (uint8_t)((MDL_PROTO_MAGIC >> 8) & 0xFFu),
            (uint8_t)((MDL_PROTO_MAGIC >> 16) & 0xFFu),
            (uint8_t)((MDL_PROTO_MAGIC >> 24) & 0xFFu),
        };

        if (b == magic_bytes[s_field_pos]) {
            s_magic_buf[s_field_pos++] = b;
            if (s_field_pos == 4) {
                s_state = RX_CMD;
                s_field_pos = 0;
            }
            return;
        }

        /* Prefix broken. Release the bytes held so far to the console,
         * then reconsider this byte as a possible fresh frame start. */
        for (uint32_t i = 0; i < s_field_pos; i++) {
            mdl_console_rx_byte(s_magic_buf[i]);
        }
        s_field_pos = 0;

        if (b == magic_bytes[0]) {
            s_magic_buf[s_field_pos++] = b;
        } else {
            mdl_console_rx_byte(b);
        }
        return;
    }

    case RX_CMD:
        s_cmd = b;
        s_state = RX_LEN;
        s_field_pos = 0;
        return;

    case RX_LEN:
        s_len_buf[s_field_pos++] = b;
        if (s_field_pos == 4) {
            s_len = (uint32_t)s_len_buf[0] | ((uint32_t)s_len_buf[1] << 8) |
                    ((uint32_t)s_len_buf[2] << 16) | ((uint32_t)s_len_buf[3] << 24);
            if (s_len > MDL_PROTO_MAX_PAYLOAD) {
                /* Garbled length field (or a genuinely oversized
                 * request) -- resync rather than try to skip s_len
                 * bytes we don't trust the count of. */
                reset_parser();
                return;
            }
            s_field_pos = 0;
            s_state = (s_len > 0) ? RX_PAYLOAD : RX_CRC;
        }
        return;

    case RX_PAYLOAD:
        s_payload[s_field_pos++] = b;
        if (s_field_pos == s_len) {
            s_field_pos = 0;
            s_state = RX_CRC;
        }
        return;

    case RX_CRC:
        s_crc_buf[s_field_pos++] = b;
        if (s_field_pos == 4) {
            uint32_t got_crc = (uint32_t)s_crc_buf[0] | ((uint32_t)s_crc_buf[1] << 8) |
                                ((uint32_t)s_crc_buf[2] << 16) | ((uint32_t)s_crc_buf[3] << 24);

            /* crc32 covers cmd + len(4) + payload, exactly what was
             * received between magic and this checksum -- reconstruct
             * that span to verify, rather than keeping a running crc
             * across states (simpler, and this runs once per frame,
             * not once per byte, so the extra buffer walk is cheap). */
            uint8_t header[5];
            header[0] = s_cmd;
            header[1] = s_len_buf[0];
            header[2] = s_len_buf[1];
            header[3] = s_len_buf[2];
            header[4] = s_len_buf[3];

            /* mdl_crc32() operates on one contiguous buffer; header and
             * payload aren't contiguous in memory here (s_len_buf vs.
             * s_payload), so verify via mdl_crc32_2() (crc32.c) instead
             * of copying them into one scratch buffer first. */
            uint32_t computed = mdl_crc32_2(header, sizeof(header), s_payload, s_len);

            if (computed == got_crc) {
                s_ready_cmd = (mdl_proto_cmd_t)s_cmd;
                s_ready_len = s_len;
                s_frame_ready = true;
                if (s_supervisor_handle_for_proto != NULL) {
                    xTaskNotifyGive(s_supervisor_handle_for_proto);
                }
            }
            /* Bad checksum: silently drop the frame and resync -- the
             * PC side times out waiting for a response and retries
             * (tools/watch.py's job), rather than this layer inventing
             * a NAK. */
            reset_parser();
        }
        return;
    }
}

bool mdl_proto_take_frame(mdl_proto_cmd_t *out_cmd, const uint8_t **out_payload, uint32_t *out_len)
{
    if (!s_frame_ready) {
        return false;
    }
    *out_cmd = s_ready_cmd;
    *out_payload = s_payload;
    *out_len = s_ready_len;
    s_frame_ready = false;
    return true;
}

void mdl_proto_send_response(mdl_proto_resp_t resp, const void *payload, uint32_t len)
{
    uint8_t header[9]; /* magic(4) + cmd(1) + len(4) -- crc(4) sent separately, after payload */
    header[0] = (uint8_t)(MDL_PROTO_MAGIC);
    header[1] = (uint8_t)(MDL_PROTO_MAGIC >> 8);
    header[2] = (uint8_t)(MDL_PROTO_MAGIC >> 16);
    header[3] = (uint8_t)(MDL_PROTO_MAGIC >> 24);
    header[4] = (uint8_t)resp;
    header[5] = (uint8_t)len;
    header[6] = (uint8_t)(len >> 8);
    header[7] = (uint8_t)(len >> 16);
    header[8] = (uint8_t)(len >> 24);

    /* crc32 covers cmd+len+payload -- same span as the RX side verifies
     * (see mdl_proto_rx_byte()'s RX_CRC case), i.e. header[4..8] here,
     * not the magic bytes. */
    uint32_t crc = mdl_crc32_2(&header[4], 5, payload, len);

    uint8_t crc_bytes[4] = {
        (uint8_t)crc, (uint8_t)(crc >> 8), (uint8_t)(crc >> 16), (uint8_t)(crc >> 24),
    };

    mdl_transport_write(header, 9);
    if (len > 0) {
        mdl_transport_write((const uint8_t *)payload, len);
    }
    mdl_transport_write(crc_bytes, 4);
}
