/*
 * M1 implementation: module runs privileged, calling straight through
 * these function pointers (no SVC trap yet -- see host_api.h's calling-
 * convention note). Parameter validation ("pointer must land inside the
 * caller's own region", "pin must be whitelisted") is only partially
 * present here: the pin whitelist check is real (cheap, and needed for
 * M1 to be a meaningful test of the vtable boundary at all), but pointer-
 * range validation is NOT implemented yet -- that's M2's "参数校验"
 * milestone, done properly once there's an SVC trap boundary to enforce
 * it at. Do not treat M1 as sandboxed against a malicious/buggy module;
 * only M2 onward is.
 */
#include <stddef.h>
#include "host_api.h"
#include "registry.h"
#include "at32f435_437.h"

/* ---- pin whitelist -------------------------------------------------- */

/* EXAMPLE whitelist -- GPIOA pins 0 and 1. Adjust to your actual board's
 * safe-to-drive pin set before running on real hardware; these are
 * placeholders, not a recommendation for any specific AT-START board
 * layout. */
typedef struct {
    gpio_type *port;
    uint16_t   mask;
} gpio_whitelist_entry_t;

static const gpio_whitelist_entry_t g_gpio_whitelist[] = {
    { GPIOA, GPIO_PINS_0 },
    { GPIOA, GPIO_PINS_1 },
};
#define GPIO_WHITELIST_COUNT (sizeof(g_gpio_whitelist) / sizeof(g_gpio_whitelist[0]))

static const gpio_whitelist_entry_t *gpio_lookup(int pin)
{
    if (pin < 0 || (size_t)pin >= GPIO_WHITELIST_COUNT) {
        return NULL;
    }
    return &g_gpio_whitelist[pin];
}

/* ---- log: ARM semihosting SYS_WRITE0 --------------------------------
 * Works under a debugger or qemu -semihosting-config enable=on without
 * needing any UART wiring decision this early. M4 adds a USB CDC sink;
 * this stays available as a fallback (e.g. for a module fault report
 * that must get out even if USB is wedged). */

static void semihost_write0(const char *msg)
{
    register uint32_t r0 __asm__("r0") = 0x04; /* SYS_WRITE0 */
    register const char *r1 __asm__("r1") = msg;
    __asm__ volatile("bkpt 0xAB" : : "r"(r0), "r"(r1) : "memory");
}

#define LOG_MAX_LEN 127

static void host_log(const char *msg)
{
    char buf[LOG_MAX_LEN + 1];
    size_t i = 0;
    while (i < LOG_MAX_LEN && msg[i] != '\0') {
        buf[i] = msg[i];
        i++;
    }
    buf[i] = '\0';
    semihost_write0(buf);
    semihost_write0("\n");
}

/* ---- gpio ------------------------------------------------------------ */

static int host_gpio_set(int pin, int level)
{
    const gpio_whitelist_entry_t *e = gpio_lookup(pin);
    if (e == NULL) {
        return -1;
    }
    gpio_bits_write(e->port, e->mask, level ? TRUE : FALSE);
    return 0;
}

static int host_gpio_get(int pin)
{
    const gpio_whitelist_entry_t *e = gpio_lookup(pin);
    if (e == NULL) {
        return -1;
    }
    return (gpio_input_data_bit_read(e->port, e->mask) == SET) ? 1 : 0;
}

/* ---- clock / delay ----------------------------------------------------
 * v1, pre-RTOS: delay_ms busy-waits on the same tick. M2 (once tasks
 * exist) replaces the busy-wait with vTaskDelay(); uptime_ms()'s
 * contract doesn't change. */

static volatile uint32_t g_tick_ms;

void SysTick_Handler(void)
{
    g_tick_ms++;
}

void host_api_clock_init(void)
{
    SysTick_Config(SystemCoreClock / 1000u);
}

static uint32_t host_uptime_ms(void)
{
    return g_tick_ms;
}

static void host_delay_ms(uint32_t ms)
{
    uint32_t start = g_tick_ms;
    while ((uint32_t)(g_tick_ms - start) < ms) {
        __NOP();
    }
}

/* ---- per-module alloc() pool -------------------------------------------
 * Carved out of the module's OWN heap+stack arena region rather than a
 * separate host-managed area -- a task only gets 3 configurable MPU
 * regions total (FreeRTOS-MPU reserves the rest for the kernel, from
 * M2 on), so reusing the region already granted to the module avoids
 * spending one of those 3 on a redundant pool. HEAP_FRACTION splits that
 * region: the bottom half backs alloc()/free(), the top half is left for
 * the module's PSP stack (stack grows down from heap_stack_hi).
 *
 * Deliberately the simplest allocator that could work: a bump pointer
 * plus one singly-linked free list, no coalescing. Good enough for v1's
 * one-module-at-a-time, reset-on-load usage; M3's "连续加载卸载 1000 轮
 * 无内存泄漏" test is what would catch this being too naive -- if it
 * ever fails that test, upgrade the allocator then, not preemptively.
 */

typedef struct free_block {
    struct free_block *next;
    size_t              size;
} free_block_t;

static uint8_t     *g_pool_next;
static uint8_t      *g_pool_end;
static free_block_t *g_free_list;

void host_api_pool_reset(module_t *m)
{
    uintptr_t region_size = m->heap_stack_hi - m->heap_stack_lo;
    g_pool_next  = (uint8_t *)m->heap_stack_lo;
    g_pool_end   = (uint8_t *)m->heap_stack_lo + region_size / 2;
    g_free_list  = NULL;
}

static void *host_alloc(size_t n)
{
    if (n == 0) {
        return NULL;
    }
    n = (n + 7u) & ~(size_t)7u; /* 8-byte align, keeps FPU/double loads happy */

    free_block_t **prev = &g_free_list;
    for (free_block_t *b = g_free_list; b != NULL; b = b->next) {
        if (b->size >= n) {
            *prev = b->next;
            return (void *)b;
        }
        prev = &b->next;
    }

    if ((size_t)(g_pool_end - g_pool_next) < n) {
        return NULL;
    }
    void *ret = g_pool_next;
    g_pool_next += n;
    return ret;
}

static void host_free(void *p)
{
    if (p == NULL) {
        return;
    }
    /* v1: trusts p was really one of ours (see the file-level note on
     * missing validation) and that its original size is >= sizeof(free_block_t) --
     * true for every allocation since host_alloc() 8-byte-aligns n and
     * free_block_t is 8 bytes on this target. */
    free_block_t *b = (free_block_t *)p;
    b->size = 0; /* unknown here; host_alloc() only checks a freed block's
                  * capacity as a best-effort reuse hint, never a hard
                  * bound -- a too-small reused block just falls through
                  * to a fresh bump allocation instead. */
    b->next = g_free_list;
    g_free_list = b;
}

/* ---- vtable ------------------------------------------------------------ */

const host_api_t g_host_api = {
    .abi_ver   = HOST_API_ABI_VERSION,
    .log       = host_log,
    .gpio_set  = host_gpio_set,
    .gpio_get  = host_gpio_get,
    .delay_ms  = host_delay_ms,
    .alloc     = host_alloc,
    .free      = host_free,
    .uptime_ms = host_uptime_ms,
};

void host_api_init(void)
{
    host_api_clock_init();
}
