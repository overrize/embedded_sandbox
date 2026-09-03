#ifndef MDL_CRC32_H
#define MDL_CRC32_H

#include <stdint.h>
#include <stddef.h>

/* Standard CRC-32 (IEEE 802.3 / zlib), reflected, poly 0xEDB88320.
 * Matches Python's zlib.crc32() bit-for-bit -- tools/packer.py uses that
 * directly, so this must stay in lockstep with it, not with any other
 * CRC-32 variant (there are several incompatible ones in the wild). */
uint32_t mdl_crc32(const void *data, size_t len);

#endif /* MDL_CRC32_H */
