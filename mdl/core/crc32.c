#include "crc32.h"

/* Bit-at-a-time (no 256-entry table) -- this runs once per module load
 * or protocol frame, not in any hot path, so the table's 1KB of flash
 * isn't worth spending here. */
static uint32_t crc32_update(uint32_t crc, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int bit = 0; bit < 8; bit++) {
            uint32_t mask = -(crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return crc;
}

uint32_t mdl_crc32(const void *data, size_t len)
{
    return ~crc32_update(0xFFFFFFFFu, data, len);
}

uint32_t mdl_crc32_2(const void *a, size_t alen, const void *b, size_t blen)
{
    uint32_t crc = 0xFFFFFFFFu;
    crc = crc32_update(crc, a, alen);
    crc = crc32_update(crc, b, blen);
    return ~crc;
}
