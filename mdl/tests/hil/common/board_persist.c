/*
 * F2: an MDL that survives power loss.
 *
 * Until now an MDL lived only in SRAM, so the board came back empty after
 * every power cycle. That is fine for development -- and it is exactly
 * wrong for a product, where the whole proposition is that a device in the
 * field keeps doing what it was told to do.
 *
 * WHAT IS STORED is the original .mdl image, not the arena contents. The
 * arena has already been relocated against a runtime base and its GOT
 * rewritten; saving that would produce something that only loads correctly
 * at the address it happened to occupy. The image is the portable thing,
 * and reloading it at boot runs the same relocation path as a fresh push,
 * so there is one loader and not two.
 *
 * WHERE it goes is the last 16K of flash, carved out of the linker's FLASH
 * region rather than assumed free past the end of the firmware. See the
 * MEMORY block in AT32F435xC_MDL_MPU.ld.
 *
 * A saved image is only used if its magic, length and CRC all agree. Flash
 * that has never been written reads as 0xFF, so an unprogrammed store fails
 * the magic check without needing a separate "is it empty" flag.
 */
#include "persist.h"
#include "crc32.h"
#include "mdl_format.h"
#include <stddef.h>
#include "at32f435_437.h"
#include "FreeRTOS.h"
#include "task.h"
#include <string.h>

extern uint32_t __mdl_store_start;
extern uint32_t __mdl_store_size;

#define STORE_ADDR ((uint32_t)(uintptr_t)&__mdl_store_start)
#define STORE_SIZE ((uint32_t)(uintptr_t)&__mdl_store_size)

/* Distinct from the .mdl file's own MDL0 magic: this one says "a valid
 * store record lives here", which is a different claim from "these bytes
 * are an MDL image". */
#define STORE_MAGIC 0x4D444C53u /* 'MDLS' */

/*
 * TWO BANKS [S5], because the store now holds a SET.
 *
 * Saving used to be: erase everything, write the one image, write the
 * header. Adding to a set cannot work that way -- the existing entries
 * live in the very sectors that would be erased, and they have to be
 * copied somewhere first.
 *
 * So the 16K splits into two 8K banks. A save writes the whole new set
 * into the INACTIVE bank while the active one stays readable, and only
 * then becomes active itself. A power cut at any point leaves the
 * previous set untouched: the half-written bank never gets its magic.
 *
 * `seq` breaks the tie. Without it, a cut between writing the new bank
 * and erasing the old would leave two valid banks and no way to say which
 * is newer -- the one failure mode a scheme like this exists to prevent.
 */
/* This part erases by sector, and the sector is 2K here. Getting it wrong
 * does not fail loudly: a too-large step leaves sectors unerased, and the
 * program that follows writes into cells that still hold old bits. */
#define SECTOR_STEP 2048u

#define BANK_SIZE   (STORE_SIZE / 2u)
#define MAX_ENTRIES 4u

typedef struct {
    uint32_t off;      /* from the bank's base */
    uint32_t len;
    uint32_t crc32;
    uint32_t _pad;     /* keeps each image 8-byte aligned */
} store_entry_t;

typedef struct {
    uint32_t magic;    /* written LAST -- see below */
    uint32_t seq;      /* higher wins */
    uint32_t count;
    uint32_t _pad;
    store_entry_t e[MAX_ENTRIES];
} store_header_t;

static flash_status_type guarded(uint32_t addr, uint32_t word, bool erase)
{
    taskENTER_CRITICAL();
    uint32_t ctrl = MPU->CTRL;
    MPU->CTRL = 0;
    __DSB();
    __ISB();

    flash_status_type st = erase ? flash_sector_erase(addr)
                                  : flash_word_program(addr, word);

    MPU->CTRL = ctrl;
    __DSB();
    __ISB();
    taskEXIT_CRITICAL();
    return st;
}

#define guarded_erase(a)      guarded((a), 0u, true)
#define guarded_program(a, w) guarded((a), (w), false)

static uint32_t bank_addr(int bank)
{
    return STORE_ADDR + ((bank == 0) ? 0u : BANK_SIZE);
}

/* A bank is valid only if its header AND every entry's CRC agree. Half a
 * good set is not a set. */
static bool bank_valid(int bank)
{
    const store_header_t *h = (const store_header_t *)bank_addr(bank);
    if (h->magic != STORE_MAGIC || h->count == 0u || h->count > MAX_ENTRIES) {
        return false;
    }
    for (uint32_t i = 0; i < h->count; i++) {
        if (h->e[i].len == 0u ||
            h->e[i].off + h->e[i].len > BANK_SIZE) {
            return false;
        }
        const uint8_t *img = (const uint8_t *)(bank_addr(bank) + h->e[i].off);
        if (mdl_crc32(img, h->e[i].len) != h->e[i].crc32) {
            return false;
        }
    }
    return true;
}

static int active_bank(void)
{
    bool v0 = bank_valid(0), v1 = bank_valid(1);
    if (v0 && v1) {
        const store_header_t *h0 = (const store_header_t *)bank_addr(0);
        const store_header_t *h1 = (const store_header_t *)bank_addr(1);
        /* Signed difference so a wrapped seq still compares correctly. */
        return ((int32_t)(h1->seq - h0->seq) > 0) ? 1 : 0;
    }
    if (v0) { return 0; }
    if (v1) { return 1; }
    return -1;
}

uint32_t board_persist_count(void)
{
    int b = active_bank();
    if (b < 0) {
        return 0u;
    }
    return ((const store_header_t *)bank_addr(b))->count;
}

const void *board_persist_entry(uint32_t i, uint32_t *out_len)
{
    int b = active_bank();
    if (b < 0) {
        return NULL;
    }
    const store_header_t *h = (const store_header_t *)bank_addr(b);
    if (i >= h->count) {
        return NULL;
    }
    *out_len = h->e[i].len;
    return (const void *)(bank_addr(b) + h->e[i].off);
}

static bool erase_bank(int bank)
{
    for (uint32_t off = 0; off < BANK_SIZE; off += SECTOR_STEP) {
        if (guarded_erase(bank_addr(bank) + off) != FLASH_OPERATE_DONE) {
            return false;
        }
    }
    return true;
}

bool board_persist_forget(void)
{
    flash_unlock();
    bool ok = erase_bank(0) && erase_bank(1);
    flash_lock();
    return ok;
}

/* Copy len bytes into flash at dst, word at a time. src may itself be in
 * flash (the other bank), which is fine: each word is read before the
 * program that follows it, never during. */
static bool write_bytes(uint32_t dst, const uint8_t *src, uint32_t len)
{
    for (uint32_t i = 0; i < len; i += 4u) {
        uint32_t word = 0xFFFFFFFFu;
        uint32_t n = (len - i >= 4u) ? 4u : (len - i);
        memcpy(&word, src + i, n);
        if (guarded_program(dst + i, word) != FLASH_OPERATE_DONE) {
            return false;
        }
    }
    return true;
}

/* The name inside a .mdl image, for replace-by-name. Read from the image
 * rather than passed in, so there is one definition of a module's
 * identity and the supervisor's slot choice cannot disagree with the
 * store's entry choice. */
static const char *image_name_of(const void *image, uint32_t len)
{
    /* offsetof rather than a hand-computed constant: a field added to
     * mdl_header_t above name would silently shift it, and the failure
     * would be "replace-by-name stopped matching" -- duplicates piling up
     * in flash with nothing reporting an error. */
    if (len < offsetof(mdl_header_t, name) + MDL_NAME_MAX) {
        return NULL;
    }
    return (const char *)((const uint8_t *)image + offsetof(mdl_header_t, name));
}

static bool same_name(const void *a, uint32_t alen, const void *b, uint32_t blen)
{
    const char *na = image_name_of(a, alen);
    const char *nb = image_name_of(b, blen);
    if (na == NULL || nb == NULL) {
        return false;
    }
    return strncmp(na, nb, MDL_NAME_MAX) == 0;
}

bool board_persist_save(const void *image, uint32_t len)
{
    if (len == 0u || len + sizeof(store_header_t) > BANK_SIZE) {
        return false;
    }

    int src_bank = active_bank();
    int dst_bank = (src_bank == 0) ? 1 : 0;
    uint32_t seq = 1u;
    const store_header_t *old = NULL;
    if (src_bank >= 0) {
        old = (const store_header_t *)bank_addr(src_bank);
        seq = old->seq + 1u;
    }

    store_header_t h;
    memset(&h, 0, sizeof(h));
    h.seq   = seq;
    h.count = 0;

    flash_unlock();
    if (!erase_bank(dst_bank)) {
        flash_lock();
        return false;
    }

    uint32_t cursor = (sizeof(store_header_t) + 7u) & ~7u;

    /* Carry over everything that is not being replaced. */
    if (old != NULL) {
        for (uint32_t i = 0; i < old->count; i++) {
            const uint8_t *img =
                (const uint8_t *)(bank_addr(src_bank) + old->e[i].off);
            if (same_name(img, old->e[i].len, image, len)) {
                continue;   /* the new one supersedes it */
            }
            if (h.count >= MAX_ENTRIES ||
                cursor + old->e[i].len > BANK_SIZE) {
                flash_lock();
                return false;
            }
            if (!write_bytes(bank_addr(dst_bank) + cursor, img, old->e[i].len)) {
                flash_lock();
                return false;
            }
            h.e[h.count].off   = cursor;
            h.e[h.count].len   = old->e[i].len;
            h.e[h.count].crc32 = old->e[i].crc32;
            h.count++;
            cursor = (cursor + old->e[i].len + 7u) & ~7u;
        }
    }

    if (h.count >= MAX_ENTRIES || cursor + len > BANK_SIZE) {
        flash_lock();
        return false;   /* full -- and the old bank is still intact */
    }
    if (!write_bytes(bank_addr(dst_bank) + cursor, (const uint8_t *)image, len)) {
        flash_lock();
        return false;
    }
    h.e[h.count].off   = cursor;
    h.e[h.count].len   = len;
    h.e[h.count].crc32 = mdl_crc32(image, len);
    h.count++;

    /* Magic last, exactly as before: a power cut before this point leaves
     * a bank that reads as absent rather than as a valid header pointing
     * at a half-written set -- and the OTHER bank is still the good one. */
    h.magic = STORE_MAGIC;
    const uint32_t *hw = (const uint32_t *)&h;
    /* Everything except the magic word first, then the magic. */
    for (uint32_t i = 1; i < sizeof(h) / 4u; i++) {
        if (guarded_program(bank_addr(dst_bank) + i * 4u, hw[i]) != FLASH_OPERATE_DONE) {
            flash_lock();
            return false;
        }
    }
    if (guarded_program(bank_addr(dst_bank), hw[0]) != FLASH_OPERATE_DONE) {
        flash_lock();
        return false;
    }

    /* The old bank can go now. Not required for correctness -- seq already
     * decides -- but leaving it would mean a later save has nowhere clean
     * to write. */
    if (src_bank >= 0) {
        (void)erase_bank(src_bank);
    }
    flash_lock();
    return true;
}
