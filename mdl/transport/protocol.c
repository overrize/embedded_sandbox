#include "protocol.h"
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
    case RX_MAGIC:
        s_magic_buf[s_field_pos++] = b;
        if (s_field_pos == 4) {
            uint32_t magic = (uint32_t)s_magic_buf[0] | ((uint32_t)s_magic_buf[1] << 8) |
                              ((uint32_t)s_magic_buf[2] << 16) | ((uint32_t)s_magic_buf[3] << 24);
            if (magic != MDL_PROTO_MAGIC) {
                /* Not a frame start -- slide the window by one byte
                 * instead of resyncing on every stray byte, so noise
                 * before a real frame doesn't need exactly 4 clean
                 * bytes to recover from. */
                s_magic_buf[0] = s_magic_buf[1];
                s_magic_buf[1] = s_magic_buf[2];
                s_magic_buf[2] = s_magic_buf[3];
                s_field_pos = 3;
                return;
            }
            s_state = RX_CMD;
            s_field_pos = 0;
        }
        return;

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
