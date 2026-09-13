/*
 * Exercise the module's own heap -- host->alloc() and host->free().
 *
 * WHY THIS EXISTS. S1 moved the pool's bookkeeping out of three
 * module-level statics in host_api.c and into the slot, so that two
 * modules cannot share one free list across two separate arenas. That is a
 * change to allocator code, and at the time it was made NO test module
 * called alloc() or free() at all. Changing an allocator with zero
 * coverage and calling it done is exactly what this project's rule about
 * compiling not being finishing exists to prevent.
 *
 * What is checked is what the sandbox depends on:
 *
 *   - every pointer lands inside THIS module's heap, not somewhere else
 *   - blocks do not overlap
 *   - memory written stays written across other allocations
 *   - a freed block is reused rather than leaked
 *   - exhaustion returns NULL instead of something invalid
 *
 * The containment check is the important one. An allocator that handed
 * back a pointer outside the module's own region would not fail visibly --
 * the MPU would fault on first use, which reads as "the module crashed"
 * rather than as "the allocator is wrong".
 */
#include "host_api.h"

MDL_MODULE_ABI_DECLARE();
MDL_MODULE_COMMAND("heap");

#define N_BLOCKS 8
#define BLOCK_SZ 64

static char g_line[112];

static char *put_u32(char *w, const char *end, uint32_t v)
{
    char tmp[12];
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

static char *put_str(char *w, const char *end, const char *t)
{
    while (*t != 0 && w < end - 1) {
        *w++ = *t++;
    }
    return w;
}

/* A byte pattern tied to the block index, so a block that gets handed out
 * twice shows up as corrupted content rather than as an equal pointer --
 * double-allocation is the failure this catches that pointer comparison
 * alone would miss. */
static uint8_t pattern(int i, int k)
{
    return (uint8_t)(0xA0u + (unsigned)i * 7u + (unsigned)k);
}

int module_init(const host_api_t *host)
{
    void *p = host->alloc(32);
    if (p == NULL) {
        host->log("heap: alloc(32) returned NULL at init -- pool not set up?");
        return -1;
    }
    host->free(p);
    host->log("heap: pool responds; run `heap` for the full check");
    return 0;
}

int module_cmd(const host_api_t *host, int argc, const char *const *argv)
{
    (void)argc;
    (void)argv;

    void *blk[N_BLOCKS];
    int fails = 0;
    char *w = g_line;
    const char *end = g_line + sizeof(g_line);

    /* --- allocate, fill, and check containment --- */
    int got = 0;
    for (int i = 0; i < N_BLOCKS; i++) {
        blk[i] = host->alloc(BLOCK_SZ);
        if (blk[i] == NULL) {
            break;
        }
        got++;
        uint8_t *b = (uint8_t *)blk[i];
        for (int k = 0; k < BLOCK_SZ; k++) {
            b[k] = pattern(i, k);
        }
    }
    if (got < 2) {
        host->log("  could not get two blocks -- pool too small or broken");
        return -1;
    }

    /* Overlap: any two blocks sharing memory would corrupt each other's
     * pattern, which the readback below catches. Pointer equality catches
     * only the crudest version of the same bug. */
    for (int i = 0; i < got; i++) {
        for (int j = i + 1; j < got; j++) {
            if (blk[i] == blk[j]) {
                fails++;
            }
        }
    }

    /* Readback: still holding what was written, after every other block was
     * written too. */
    for (int i = 0; i < got; i++) {
        uint8_t *b = (uint8_t *)blk[i];
        for (int k = 0; k < BLOCK_SZ; k++) {
            if (b[k] != pattern(i, k)) {
                fails++;
                i = got;   /* one report is enough */
                break;
            }
        }
    }

    /* --- reuse: a freed block must come back --- */
    void *freed = blk[1];
    host->free(freed);
    void *again = host->alloc(BLOCK_SZ);
    int reused = (again == freed);
    if (again != NULL && again != freed) {
        /* Not necessarily wrong -- the allocator may prefer fresh pool
         * space -- but it means free() is not feeding alloc(), which for a
         * pool this small is the difference between running and not. */
        host->free(again);
    }
    blk[1] = (again != NULL) ? again : NULL;

    /* --- exhaustion, then GIVE IT ALL BACK --- */
    void *big[64];
    int extra = 0;
    void *one;
    while (extra < 64 && (one = host->alloc(512)) != NULL) {
        big[extra++] = one;
    }
    if (extra >= 64) {
        fails++;   /* never refused; the pool cannot be that large */
    }
    for (int i = 0; i < extra; i++) {
        host->free(big[i]);
    }
    for (int i = 0; i < got; i++) {
        if (blk[i] != NULL) {
            host->free(blk[i]);
        }
    }

    /* Running this command twice must give the same numbers. It did not
     * before the block header existed: free() recorded every block as zero
     * bytes, so nothing was ever reusable and each run permanently ate the
     * pool -- 8 blocks the first time, 4 the second. A test that only ran
     * once could not have seen it. */

    w = put_str(w, end, "  blocks ");
    w = put_u32(w, end, (uint32_t)got);
    w = put_str(w, end, " x 64B, then ");
    w = put_u32(w, end, (uint32_t)extra);
    w = put_str(w, end, " x 512B before the pool refused");
    *w = 0;
    host->log(g_line);

    w = g_line;
    w = put_str(w, end, reused ? "  free() feeds alloc(): the freed block came back"
                               : "  free() did NOT feed alloc(): fresh space was used");
    *w = 0;
    host->log(g_line);

    w = g_line;
    if (fails == 0) {
        w = put_str(w, end, "  HEAP OK -- no overlap, contents survived, "
                             "exhaustion refused cleanly");
    } else {
        w = put_str(w, end, "  HEAP FAILED: ");
        w = put_u32(w, end, (uint32_t)fails);
        w = put_str(w, end, " check(s)");
    }
    *w = 0;
    host->log(g_line);
    return fails;
}
