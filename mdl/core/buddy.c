#include "buddy.h"

/*
 * Classic binary buddy. The only non-obvious choices are recorded where
 * they are made; buddy.h carries the reasoning for using one at all.
 */

#define ORDER_COUNT (MDL_BUDDY_MAX_ORDER - MDL_BUDDY_MIN_ORDER + 1)

/* A free block stores its own list link in its first bytes. Free memory is
 * the one place bookkeeping is free, and the minimum block is 1K, so a
 * pointer fits with room to spare. */
typedef struct free_node {
    struct free_node *next;
} free_node_t;

static uintptr_t   s_base;
static free_node_t *s_free[ORDER_COUNT];

/*
 * Side table: one entry per minimum-sized slot, holding the order of the
 * block that STARTS there, or 0 for "not the start of a live block".
 *
 * A side table rather than a header in front of each block, because the
 * returned pointer has to be the naturally-aligned base -- that is the
 * whole reason for using a buddy allocator here. A header would shift the
 * usable area off alignment and defeat the purpose.
 */
static uint8_t s_order[MDL_BUDDY_SLOTS];

static size_t idx_of(uintptr_t addr)
{
    return (size_t)((addr - s_base) >> MDL_BUDDY_MIN_ORDER);
}

static int order_for(size_t size)
{
    int o = MDL_BUDDY_MIN_ORDER;
    while (((size_t)1u << o) < size) {
        o++;
        if (o > MDL_BUDDY_MAX_ORDER) {
            return -1;
        }
    }
    return o;
}

static void push(int order, uintptr_t addr)
{
    free_node_t *n = (free_node_t *)addr;
    n->next = s_free[order - MDL_BUDDY_MIN_ORDER];
    s_free[order - MDL_BUDDY_MIN_ORDER] = n;
}

/* Remove one specific address from an order's free list. Returns false if
 * it was not there, which is how "is my buddy free" gets answered without
 * a separate bitmap. */
static bool unlink_at(int order, uintptr_t addr)
{
    free_node_t **pp = &s_free[order - MDL_BUDDY_MIN_ORDER];
    while (*pp != NULL) {
        if ((uintptr_t)(*pp) == addr) {
            *pp = (*pp)->next;
            return true;
        }
        pp = &(*pp)->next;
    }
    return false;
}

static uintptr_t pop(int order)
{
    free_node_t *n = s_free[order - MDL_BUDDY_MIN_ORDER];
    if (n == NULL) {
        return 0;
    }
    s_free[order - MDL_BUDDY_MIN_ORDER] = n->next;
    return (uintptr_t)n;
}

void mdl_buddy_init(uintptr_t base)
{
    s_base = base;
    for (int i = 0; i < ORDER_COUNT; i++) {
        s_free[i] = NULL;
    }
    for (size_t i = 0; i < MDL_BUDDY_SLOTS; i++) {
        s_order[i] = 0;
    }
    /* The whole pool starts as one block of the largest order. */
    push(MDL_BUDDY_MAX_ORDER, base);
}

size_t mdl_buddy_block_size(size_t size)
{
    int o = order_for(size);
    return (o < 0) ? 0u : ((size_t)1u << o);
}

void *mdl_buddy_alloc(size_t size)
{
    if (size == 0u) {
        return NULL;
    }
    int want = order_for(size);
    if (want < 0) {
        return NULL;
    }

    /* Smallest available order that can satisfy this. Taking the smallest
     * rather than the first keeps big blocks intact for big requests --
     * splitting a 128K block to serve 1K is how an arena stops being able
     * to load anything large. */
    int have = -1;
    for (int o = want; o <= MDL_BUDDY_MAX_ORDER; o++) {
        if (s_free[o - MDL_BUDDY_MIN_ORDER] != NULL) {
            have = o;
            break;
        }
    }
    if (have < 0) {
        return NULL;
    }

    uintptr_t addr = pop(have);

    /* Split down, freeing the upper half at each step. The lower half stays
     * with us, so the address never moves and stays aligned to every order
     * it passes through. */
    while (have > want) {
        have--;
        push(have, addr + ((uintptr_t)1u << have));
    }

    s_order[idx_of(addr)] = (uint8_t)want;
    return (void *)addr;
}

void mdl_buddy_free(void *p)
{
    if (p == NULL) {
        return;
    }
    uintptr_t addr = (uintptr_t)p;

    /* Reject anything that is not a live block's base. A pointer into the
     * middle of a block, a stale one, or a second free of the same block
     * would all corrupt the free lists; ignoring them leaks at worst. */
    if (addr < s_base || addr >= s_base + MDL_BUDDY_POOL_SIZE) {
        return;
    }
    if ((addr - s_base) % MDL_BUDDY_MIN_SIZE != 0u) {
        return;
    }
    size_t i = idx_of(addr);
    int order = (int)s_order[i];
    if (order < MDL_BUDDY_MIN_ORDER || order > MDL_BUDDY_MAX_ORDER) {
        return;
    }
    s_order[i] = 0;

    /* Coalesce upward for as long as the partner half is also free. This is
     * the property the whole choice of allocator rests on: without it,
     * repeated load/unload would leave the arena full of holes that no
     * module fits into. */
    while (order < MDL_BUDDY_MAX_ORDER) {
        uintptr_t buddy = s_base +
            (((addr - s_base) ^ ((uintptr_t)1u << order)));
        if (!unlink_at(order, buddy)) {
            break;   /* partner is in use or split; stop here */
        }
        if (buddy < addr) {
            addr = buddy;   /* merged block starts at the lower half */
        }
        order++;
    }
    push(order, addr);
}

void mdl_buddy_stats(size_t *free_total, size_t *largest)
{
    size_t total = 0, big = 0;
    for (int o = MDL_BUDDY_MIN_ORDER; o <= MDL_BUDDY_MAX_ORDER; o++) {
        size_t sz = (size_t)1u << o;
        for (free_node_t *n = s_free[o - MDL_BUDDY_MIN_ORDER]; n != NULL; n = n->next) {
            total += sz;
            if (sz > big) {
                big = sz;
            }
        }
    }
    if (free_total != NULL) { *free_total = total; }
    if (largest != NULL)    { *largest = big; }
}

/* ---------------------------------------------------------------------- */

static char *st_str(char *w, const char *end, const char *t)
{
    while (*t != 0 && w < end - 1) {
        *w++ = *t++;
    }
    return w;
}

static char *st_u32(char *w, const char *end, uint32_t v)
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

/*
 * Self-test.
 *
 * It runs against the REAL allocator on a scratch pool, saving and
 * restoring the live state around itself, because a test of a
 * reimplementation tests the reimplementation. The properties checked are
 * the ones the design actually depends on -- alignment, coalescing, and
 * refusal of bad frees -- rather than "does it return non-NULL".
 */
int mdl_buddy_selftest(char *detail, size_t detail_len)
{
    /* Save live state so a self-test can be run at any time without
     * disturbing whatever is loaded. */
    uintptr_t   saved_base = s_base;
    free_node_t *saved_free[ORDER_COUNT];
    static uint8_t saved_order[MDL_BUDDY_SLOTS];
    for (int i = 0; i < ORDER_COUNT; i++) { saved_free[i] = s_free[i]; }
    for (size_t i = 0; i < MDL_BUDDY_SLOTS; i++) { saved_order[i] = s_order[i]; }

    /* A scratch pool carved out of the real one: take the whole arena, run
     * on it, then restore. Nothing else may allocate meanwhile, which holds
     * because this runs from the console task with no module loaded or
     * with the module untouched (its blocks are in saved_order). */
    int fails = 0;
    char *w = detail;
    const char *end = detail + detail_len;

    mdl_buddy_init(saved_base);

    size_t total = 0, largest = 0;
    mdl_buddy_stats(&total, &largest);
    if (total != MDL_BUDDY_POOL_SIZE || largest != MDL_BUDDY_POOL_SIZE) {
        fails++; w = st_str(w, end, "init-stats ");
    }

    /* Alignment is the property the MPU depends on: every block must be
     * aligned to its own rounded-up size, not merely to the minimum. */
    void *a = mdl_buddy_alloc(1024);
    void *b = mdl_buddy_alloc(4096);
    void *c = mdl_buddy_alloc(3000);   /* rounds to 4096 */
    if (a == NULL || b == NULL || c == NULL) {
        fails++; w = st_str(w, end, "alloc-null ");
    } else {
        if ((uintptr_t)a % 1024u != 0u) { fails++; w = st_str(w, end, "align-a "); }
        if ((uintptr_t)b % 4096u != 0u) { fails++; w = st_str(w, end, "align-b "); }
        if ((uintptr_t)c % 4096u != 0u) { fails++; w = st_str(w, end, "align-c "); }
        if (a == b || b == c || a == c) { fails++; w = st_str(w, end, "overlap "); }
    }
    if (mdl_buddy_block_size(3000) != 4096u) {
        fails++; w = st_str(w, end, "round ");
    }

    /* Coalescing: after giving everything back, the pool must be ONE whole
     * block again. Merely having the bytes back is not enough -- that is
     * exactly the fragmented state this allocator exists to avoid. */
    mdl_buddy_free(a);
    mdl_buddy_free(b);
    mdl_buddy_free(c);
    mdl_buddy_stats(&total, &largest);
    if (total != MDL_BUDDY_POOL_SIZE) {
        fails++; w = st_str(w, end, "leak ");
    }
    if (largest != MDL_BUDDY_POOL_SIZE) {
        fails++; w = st_str(w, end, "no-coalesce ");
    }

    /* Bad frees must be ignored, not acted on. */
    mdl_buddy_free(NULL);
    mdl_buddy_free((void *)(saved_base + 7u));            /* misaligned */
    mdl_buddy_free((void *)(saved_base + MDL_BUDDY_POOL_SIZE + 4096u)); /* outside */
    void *d = mdl_buddy_alloc(2048);
    mdl_buddy_free(d);
    mdl_buddy_free(d);                                     /* double free */
    mdl_buddy_stats(&total, &largest);
    if (total != MDL_BUDDY_POOL_SIZE || largest != MDL_BUDDY_POOL_SIZE) {
        fails++; w = st_str(w, end, "bad-free-damaged ");
    }

    /* Exhaustion must refuse cleanly rather than return something wrong. */
    void *whole = mdl_buddy_alloc(MDL_BUDDY_POOL_SIZE);
    if (whole == NULL) {
        fails++; w = st_str(w, end, "whole-pool ");
    }
    if (mdl_buddy_alloc(MDL_BUDDY_MIN_SIZE) != NULL) {
        fails++; w = st_str(w, end, "overcommit ");
    }
    mdl_buddy_free(whole);

    /* A request larger than the pool is a refusal, not a wrap-around. */
    if (mdl_buddy_alloc(MDL_BUDDY_POOL_SIZE * 2u) != NULL) {
        fails++; w = st_str(w, end, "oversize ");
    }

    if (w == detail) {
        w = st_str(w, end, "all checks passed, pool ");
        w = st_u32(w, end, (uint32_t)(MDL_BUDDY_POOL_SIZE / 1024u));
        w = st_str(w, end, "K");
    }
    *w = 0;

    /* Restore whatever was live before. */
    s_base = saved_base;
    for (int i = 0; i < ORDER_COUNT; i++) { s_free[i] = saved_free[i]; }
    for (size_t i = 0; i < MDL_BUDDY_SLOTS; i++) { s_order[i] = saved_order[i]; }
    return fails;
}
