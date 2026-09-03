#include <string.h>
#include "loader.h"
#include "crc32.h"
#include "arch_if.h"

/*
 * Runtime memory layout of the module's DATA arena region (m->data_lo
 * .. m->data_hi), decided here and nowhere else:
 *
 *   [ GOT  (header.got_count * 4 bytes) ]   <- r9 points here
 *   [ data (header.data_size bytes)     ]
 *   [ bss  (header.bss_size bytes, zeroed) ]
 *
 * This does NOT match the on-disk payload order ([text][data][got][reloc]
 * -- see mdl_header_t's comment in mdl_format.h); the packer's job is to
 * produce offsets consistent with THIS layout (GOT first), not with the
 * file's own byte order. If this layout ever changes, packer.py's
 * offset computation must change with it -- they are one contract split
 * across two languages, not independently variable.
 */

const char *mdl_load_status_str(mdl_load_status_t status)
{
    switch (status) {
    case MDL_LOAD_OK:                return "ok";
    case MDL_LOAD_ERR_TOO_SMALL:     return "image smaller than header";
    case MDL_LOAD_ERR_BAD_MAGIC:     return "bad magic (not an MDL0 image)";
    case MDL_LOAD_ERR_BAD_CRC:       return "crc32 mismatch (corrupt transfer?)";
    case MDL_LOAD_ERR_ABI_MISMATCH:  return "abi_ver mismatch";
    case MDL_LOAD_ERR_ARCH_MISMATCH: return "arch mismatch";
    case MDL_LOAD_ERR_TEXT_TOO_BIG:  return "text section exceeds arena text region";
    case MDL_LOAD_ERR_DATA_TOO_BIG:  return "got+data+bss exceeds arena data region";
    case MDL_LOAD_ERR_BAD_RELOC:     return "reloc entry points outside the GOT area";
    }
    return "unknown status";
}

mdl_load_status_t mdl_load(module_t *m, const void *image, size_t image_len,
                            uint16_t expected_abi_ver, mdl_arch_t expected_arch)
{
    if (image_len < sizeof(mdl_header_t)) {
        return MDL_LOAD_ERR_TOO_SMALL;
    }

    const mdl_header_t *hdr = (const mdl_header_t *)image;
    if (hdr->magic != MDL_MAGIC) {
        return MDL_LOAD_ERR_BAD_MAGIC;
    }

    const uint8_t *payload = (const uint8_t *)image + sizeof(mdl_header_t);
    size_t payload_len = image_len - sizeof(mdl_header_t);
    if (mdl_crc32(payload, payload_len) != hdr->crc32) {
        return MDL_LOAD_ERR_BAD_CRC;
    }

    if (hdr->abi_ver != expected_abi_ver) {
        return MDL_LOAD_ERR_ABI_MISMATCH;
    }
    if (hdr->arch != (uint16_t)expected_arch) {
        return MDL_LOAD_ERR_ARCH_MISMATCH;
    }

    if (hdr->text_size > (size_t)(m->text_hi - m->text_lo)) {
        return MDL_LOAD_ERR_TEXT_TOO_BIG;
    }

    size_t got_bytes = (size_t)hdr->got_count * 4u;
    size_t data_region_needed = got_bytes + hdr->data_size + hdr->bss_size;
    if (data_region_needed > (size_t)(m->data_hi - m->data_lo)) {
        return MDL_LOAD_ERR_DATA_TOO_BIG;
    }

    /* Payload layout on disk: [text][data][got][reloc_table]. Offsets
     * below are all relative to `payload` (i.e. past the header), per
     * mdl_header_t's own field comments. */
    const uint8_t *text_src  = payload;
    const uint8_t *data_src  = payload + hdr->text_size;
    const uint8_t *got_src   = payload + hdr->got_off;
    const mdl_reloc_t *reloc_src = (const mdl_reloc_t *)(payload + hdr->reloc_off);

    for (uint32_t i = 0; i < hdr->reloc_count; i++) {
        if (reloc_src[i].got_offset + 4u > got_bytes) {
            return MDL_LOAD_ERR_BAD_RELOC;
        }
    }

    uint8_t *text_dst = (uint8_t *)m->text_lo;
    uint8_t *got_dst   = (uint8_t *)m->data_lo;
    uint8_t *data_dst  = got_dst + got_bytes;
    uint8_t *bss_dst   = data_dst + hdr->data_size;

    memcpy(text_dst, text_src, hdr->text_size);
    memcpy(data_dst, data_src, hdr->data_size);
    memset(bss_dst, 0, hdr->bss_size);
    memcpy(got_dst, got_src, got_bytes);

    /* TODO(post-v1): run .init_array here, before arch_apply_relocs()
     * would be premature anyway since ctors could themselves take the
     * addresses this relocates -- actually ctors should run AFTER
     * relocation, right before module_init(). mdl_header_t has no
     * init_array_off/count field in v1's wire format (this loader can't
     * invent bounds it wasn't given), so this hook is a no-op for now;
     * adding those two header fields is the natural way to wire it up
     * later without touching anything else in this function. */

    arch_apply_relocs(m, reloc_src, hdr->reloc_count);
    arch_code_sync(text_dst, hdr->text_size);

    m->entry = text_dst + hdr->init_off;
    m->state = MDL_SLOT_LOADED;
    return MDL_LOAD_OK;
}

int mdl_run(module_t *m, const struct host_api *host)
{
    void *got_base = (void *)m->data_lo;
    int ret = arch_call_privileged(m->entry, got_base, host);
    m->state = MDL_SLOT_RUNNING;
    return ret;
}
