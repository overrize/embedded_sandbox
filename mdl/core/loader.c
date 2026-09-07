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

static char *detail_put(char *w, const char *end, const char *text)
{
    while (*text != '\0' && w < end - 1) {
        *w++ = *text++;
    }
    return w;
}

static char *detail_put_u32(char *w, const char *end, uint32_t v)
{
    char tmp[11];
    int n = 0;
    do {
        tmp[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    } while (v != 0u && n < (int)sizeof(tmp));
    while (n-- > 0 && w < end - 1) {
        *w++ = tmp[n];
    }
    return w;
}

/*
 * Who already owns a piece of hardware, or NULL if it is free.
 *
 * Weak because core/ has no idea what hardware exists -- that is the
 * host layer's business (mdl/host/host_api.c supplies the real one). A
 * build without a host layer keeps this stub, and then nothing is owned
 * and every well-formed claim is granted, which is the right behaviour
 * for M0/M1-style bare targets that have no competing host tasks.
 */
__attribute__((weak)) const char *mdl_res_owner(uint8_t kind, uint8_t id)
{
    (void)kind;
    (void)id;
    return NULL;
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

    for (uint32_t i = 0; i < count; i++) {
        if (res[i].kind != (uint8_t)MDL_RES_KIND_GPIO) {
            return MDL_LOAD_ERR_BAD_RES;
        }
        /* 32 because gpio_claimed is a uint32_t bitmap; the host
         * whitelist is far shorter than that, and an id past its end is
         * caught by mdl_res_owner() returning a not-whitelisted owner. */
        if (res[i].id >= 32u) {
            return MDL_LOAD_ERR_BAD_RES;
        }

        const char *owner = mdl_res_owner(res[i].kind, res[i].id);
        if (owner != NULL) {
            char *w = g_detail;
            const char *end = g_detail + sizeof(g_detail);
            w = detail_put(w, end, "gpio ");
            w = detail_put_u32(w, end, res[i].id);
            w = detail_put(w, end, " is owned by ");
            w = detail_put(w, end, owner);
            *w = '\0';
            return MDL_LOAD_ERR_RES_CONFLICT;
        }
        claimed |= (1u << res[i].id);
    }

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
