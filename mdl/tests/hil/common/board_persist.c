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

typedef struct {
    uint32_t magic;
    uint32_t length;
    uint32_t crc32;   /* over the image bytes that follow */
    uint32_t _pad;    /* keeps the image 8-byte aligned */
} store_header_t;

/* Smallest erasable unit on this part. Erasing by stepping this far is
 * safe even if the real sector is larger -- flash_sector_erase() erases
 * whichever sector contains the address, so a repeat is wasted work, not
 * damage. Being wrong in the other direction would leave a sector
 * unerased and the program silently corrupt. */
#define SECTOR_STEP 2048u

/*
 * Programming flash means STORING TO A FLASH ADDRESS on this part, and
 * FreeRTOS-MPU maps the whole flash region read-only -- correctly, since
 * it is code. So a save faults with DACCVIOL at the store address, and
 * the fault record says exactly that.
 *
 * Shrinking the MPU's flash region to exclude the store does not work:
 * region sizes are powers of two, so 240K rounds up to 256K and covers it
 * again.
 *
 * The first attempt disabled the MPU around the whole erase-and-program
 * and failed identically, for a reason worth recording: FreeRTOS's PendSV
 * re-enables the MPU on EVERY context switch (it clears and sets
 * MPU_CTRL.ENABLE around restoring a task's regions). Leaving interrupts
 * on to keep USB alive is precisely what let a context switch happen mid-
 * window and turn protection back on underneath the write.
 *
 * So each window covers ONE flash operation and holds off the scheduler
 * for its duration:
 *   - taskENTER_CRITICAL() masks up to configMAX_SYSCALL_INTERRUPT_PRIORITY
 *     (5), which includes PendSV, so no context switch can re-enable the
 *     MPU while the window is open.
 *   - The USB interrupt is priority 0, above that threshold, so it keeps
 *     running and the CDC link survives even the ~30ms of a sector erase.
 *     (It must not call the FreeRTOS API, and it does not -- see
 *     usb_cdc.c.)
 *   - A word program is tens of microseconds, so those windows are noise.
 */
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
const void *board_persist_image(uint32_t *out_len)
{
    const store_header_t *h = (const store_header_t *)STORE_ADDR;
    const uint8_t *image = (const uint8_t *)(STORE_ADDR + sizeof(store_header_t));

    if (h->magic != STORE_MAGIC) {
        return NULL;   /* never written, or erased */
    }
    if (h->length == 0u || h->length > STORE_SIZE - sizeof(store_header_t)) {
        return NULL;
    }
    if (mdl_crc32(image, h->length) != h->crc32) {
        return NULL;   /* interrupted write, or bit rot */
    }
    *out_len = h->length;
    return image;
}

static bool erase_store(void)
{
    for (uint32_t off = 0; off < STORE_SIZE; off += SECTOR_STEP) {
        if (guarded_erase(STORE_ADDR + off) != FLASH_OPERATE_DONE) {
            return false;
        }
    }
    return true;
}

bool board_persist_forget(void)
{
    flash_unlock();
    bool ok = erase_store();
    flash_lock();
    return ok;
}

bool board_persist_save(const void *image, uint32_t len)
{
    if (len == 0u || len > STORE_SIZE - sizeof(store_header_t)) {
        return false;
    }

    store_header_t h;
    h.magic  = STORE_MAGIC;
    h.length = len;
    h.crc32  = mdl_crc32(image, len);
    h._pad   = 0;

    flash_unlock();
    if (!erase_store()) {
        flash_lock();
            return false;
    }

    /* Image first, header last. A power cut mid-write then leaves the
     * magic unwritten, so the record reads as absent rather than as a
     * valid header pointing at a half-written image -- the difference
     * between booting empty and booting into garbage. */
    const uint8_t *src = (const uint8_t *)image;
    uint32_t addr = STORE_ADDR + sizeof(store_header_t);
    for (uint32_t i = 0; i < len; i += 4u) {
        uint32_t word = 0xFFFFFFFFu;
        uint32_t n = (len - i >= 4u) ? 4u : (len - i);
        memcpy(&word, src + i, n);
        if (guarded_program(addr + i, word) != FLASH_OPERATE_DONE) {
            flash_lock();
                    return false;
        }
    }

    const uint32_t *hw = (const uint32_t *)&h;
    for (uint32_t i = 0; i < sizeof(h) / 4u; i++) {
        if (guarded_program(STORE_ADDR + i * 4u, hw[i]) != FLASH_OPERATE_DONE) {
            flash_lock();
                    return false;
        }
    }
    flash_lock();

    uint32_t check_len = 0;
    return board_persist_image(&check_len) != NULL && check_len == len;
}

uint32_t board_persist_capacity(void)
{
    return STORE_SIZE - (uint32_t)sizeof(store_header_t);
}
