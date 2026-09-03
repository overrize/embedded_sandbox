#include "crc32.h"

/* Bit-at-a-time (no 256-entry table) -- this runs once per module load,
 * not in any hot path, so the table's 1KB of flash isn't worth spending
 * here. */
uint32_t mdl_crc32(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFu;

    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int bit = 0; bit < 8; bit++) {
            uint32_t mask = -(crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}
