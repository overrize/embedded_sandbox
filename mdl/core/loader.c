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
    case MDL_LOAD_ERR_BAD_RES:       return "malformed resource claim";
    case MDL_LOAD_ERR_RES_CONFLICT:  return "resource conflict";
    }
    return "unknown status";
}

/*
 * Detail for the one failure whose useful message is not a constant:
 * a resource conflict has to name the pin AND its current owner, or the
 * person reading `RESP_ERROR resource conflict` learns nothing they
 * could act on.
 */
static char g_detail[80];

const char *mdl_load_detail(void)
{
    return g_detail;
}

/*
 * The whole resource decision, answered by the host.
 *
 * Weak because core/ has no idea what hardware exists, let alone which
 * physical pins a peripheral occupies -- and pins are the only space in
 * which conflicts are decidable (mdl/host/host_resources.c explains why).
 * A build with no host layer keeps this stub and grants everything, which
 * is right for the bare M0/M1 targets that have no competing hardware.
 */
__attribute__((weak)) bool mdl_res_check(const mdl_res_t *res, uint32_t count,
                                          char *detail, uint32_t detail_len)
{
    (void)res;
    (void)count;
    (void)detail_len;
    detail[0] = 0;
    return true;
}

/*
 * Runs BEFORE anything is copied into the arena, on purpose: a module
 * that is going to be rejected must not have left half of itself in the
 * text region first. Fills g_detail on conflict.
 */
static mdl_load_status_t check_resources(module_t *m, const mdl_res_t *res,
                                          uint32_t count)
{
    uint32_t claimed = 0;

    if (count > MDL_MAX_RES) {
        return MDL_LOAD_ERR_BAD_RES;
    }

    for (uint32_t i = 0; i < count; i++) {
        if (res[i].kind == (uint8_t)MDL_RES_KIND_NONE ||
            res[i].kind > (uint8_t)MDL_RES_KIND_TIMER) {
            return MDL_LOAD_ERR_BAD_RES;
        }
        /* 32 because gpio_claimed is a uint32_t bitmap; the host
         * whitelist is far shorter than that, and an id past its end is
         * caught by mdl_res_owner() returning a not-whitelisted owner. */
        /* 32 bounds the gpio_claimed bitmap; peripheral instance numbers
         * are validated by the host knowing them or not. */
        if (res[i].kind == (uint8_t)MDL_RES_KIND_GPIO && res[i].id >= 32u) {
            return MDL_LOAD_ERR_BAD_RES;
        }

        if (res[i].kind == (uint8_t)MDL_RES_KIND_GPIO) {
            claimed |= (1u << res[i].id);
        }
        /* Kept verbatim: a bitmap loses the edge selection, and the
         * supervisor needs it to arm the interrupt after the load. */
        m->res[i] = res[i];
    }

    /* One call, after the per-claim sanity checks: the host expands every
     * claim to physical pins and compares THOSE, which is the only way
     * two differently-named claims on one pin get noticed. */
    if (!mdl_res_check(res, count, g_detail, (uint32_t)sizeof(g_detail))) {
        return MDL_LOAD_ERR_RES_CONFLICT;
    }

    m->res_count    = (uint8_t)count;
    m->gpio_claimed = claimed;
    return MDL_LOAD_OK;
}

mdl_load_status_t mdl_load(module_t *m, const void *image, size_t image_len,
                            uint16_t expected_abi_ver, mdl_arch_t expected_arch)
{
    g_detail[0] = '\0';

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

    /* Resource arbitration, still before the first memcpy. */
    if ((size_t)hdr->res_off + (size_t)hdr->res_count * sizeof(mdl_res_t) > payload_len) {
        return MDL_LOAD_ERR_BAD_RES;
    }
    /* The packer already refused an over-budget declaration; this is the
     * defensive copy of that check, for an image that did not come
     * through our packer. */
    if (hdr->evt_queue_depth > MDL_EVT_QUEUE_MAX) {
        return MDL_LOAD_ERR_BAD_RES;
    }

    mdl_load_status_t res_st = check_resources(
        m, (const mdl_res_t *)(payload + hdr->res_off), hdr->res_count);
    if (res_st != MDL_LOAD_OK) {
        m->gpio_claimed = 0;
        return res_st;
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

    for (uint32_t i = 0; i < MDL_NAME_MAX; i++) {
        m->name[i] = hdr->name[i];
    }
    m->name[MDL_NAME_MAX - 1u] = '\0';

    /* cmd_off carries the Thumb bit exactly like init_off, so a module
     * that exports no command has cmd_off == 0 and is unambiguous. */
    if (hdr->cmd_off != 0u) {
        m->cmd_entry = text_dst + hdr->cmd_off;
        for (uint32_t i = 0; i < MDL_CMD_NAME_MAX; i++) {
            m->cmd_name[i] = hdr->cmd_name[i];
        }
        m->cmd_name[MDL_CMD_NAME_MAX - 1u] = '\0';
    } else {
        m->cmd_entry = NULL;
        m->cmd_name[0] = '\0';
    }

    m->evt_entry = (hdr->evt_off != 0u) ? (text_dst + hdr->evt_off) : NULL;
    m->evt_queue_depth = hdr->evt_queue_depth;
    m->evt_rate_hz     = hdr->evt_rate_hz;

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
