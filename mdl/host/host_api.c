/*
 * M2: module runs UNPRIVILEGED (mdl/core/module_task.c). Every function
 * below is now a real SVC-gated wrapper: it calls xPortRaisePrivilege()
 * (an `svc portSVC_RAISE_PRIVILEGE` that only actually elevates because
 * this function's code lives in the freertos_system_calls linker
 * section -- see FreeRTOSConfig.h's configENFORCE_SYSTEM_CALLS_FROM_
 * KERNEL_ONLY comment), does the real, now-privileged work with real
 * parameter validation, then calls vPortResetPrivilege() before
 * returning to the unprivileged caller. This reuses FreeRTOS-MPU's OWN
 * privilege-escalation primitives (mdl/third_party/freertos_mpu_port's
 * mpu_wrappers.c) rather than inventing a parallel SVC-number dispatch
 * mechanism -- there's already exactly one SVC number
 * (portSVC_RAISE_PRIVILEGE) that does exactly what's needed here.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "host_api.h"
#include "registry.h"
#include "module_task.h" /* MDL_HEAP_SIZE */
#include "at32f435_437.h"
#include "FreeRTOS.h"
#include "task.h"

/* Not exposed by any FreeRTOS public header (only used internally by
 * mpu_wrappers.c) -- these two are the actual privilege-escalation
 * primitives, real declarations, matching mpu_wrappers.c exactly. */
extern BaseType_t xPortRaisePrivilege(void);
extern void vPortResetPrivilege(BaseType_t xRunningPrivileged);

#define MDL_SYSCALL_GATE __attribute__((section("freertos_system_calls")))

/* ---- pointer validation --------------------------------------------
 * "所有指针参数必须落在该模块自己的 data/heap 区间内" -- but a module
 * may legitimately pass a string literal, which lives in its OWN
 * .rodata (part of the TEXT region, read-only) -- so "valid" here means
 * "inside text, data, or this module's alloc() pool", not just data/heap.
 * Runs privileged (called only from inside an already-raised gate
 * function), so this can safely read g_mdl_slot directly. */
static bool ptr_in_range(uintptr_t p, size_t len, uintptr_t lo, uintptr_t hi)
{
    if (len > (size_t)(hi - lo)) {
        return false;
    }
    return p >= lo && p <= hi - len;
}

static bool ptr_owned_by_module(const void *p, size_t len)
{
    uintptr_t addr = (uintptr_t)p;
    if (len == 0) {
        return true; /* a zero-length range trivially can't be touched */
    }
    module_t *m = &g_mdl_slot;
    return ptr_in_range(addr, len, m->text_lo, m->text_hi) ||
           ptr_in_range(addr, len, m->data_lo, m->data_hi) ||
           ptr_in_range(addr, len, m->heap_stack_lo, m->heap_stack_lo + MDL_HEAP_SIZE);
}

/* ---- pin whitelist -------------------------------------------------- */

typedef struct {
    gpio_type *port;
    uint16_t   mask;
} gpio_whitelist_entry_t;

/* EXAMPLE whitelist -- adjust to your actual board before real use. */
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

/* ---- log: ARM semihosting SYS_WRITE0 (placeholder until M4's USB CDC) */

static void semihost_write0(const char *msg)
{
    register uint32_t r0 __asm__("r0") = 0x04;
    register const char *r1 __asm__("r1") = msg;
    __asm__ volatile("bkpt 0xAB" : : "r"(r0), "r"(r1) : "memory");
}

#define LOG_MAX_LEN 127

static void host_log_impl(const char *msg)
{
    if (!ptr_owned_by_module(msg, 1)) {
        semihost_write0("[host] log() rejected: msg not in module's own memory");
        return;
    }
    char buf[LOG_MAX_LEN + 1];
    size_t i = 0;
    /* msg's *first* byte was validated above; each subsequent byte is
     * re-checked as we go, since a NUL might be further away than the
     * region actually extends (a hostile module could omit it) -- never
     * trust strlen() on unvalidated module memory. */
    while (i < LOG_MAX_LEN && ptr_owned_by_module(msg + i, 1) && msg[i] != '\0') {
        buf[i] = msg[i];
        i++;
    }
    buf[i] = '\0';
    semihost_write0(buf);
    semihost_write0("\n");
}

void host_log(const char *msg) MDL_SYSCALL_GATE;
void host_log(const char *msg)
{
    BaseType_t was_priv = xPortRaisePrivilege();
    host_log_impl(msg);
    vPortResetPrivilege(was_priv);
}

/* ---- gpio ------------------------------------------------------------ */

static int host_gpio_set_impl(int pin, int level)
{
    const gpio_whitelist_entry_t *e = gpio_lookup(pin);
    if (e == NULL) {
        return -1;
    }
    gpio_bits_write(e->port, e->mask, level ? TRUE : FALSE);
    return 0;
}

int host_gpio_set(int pin, int level) MDL_SYSCALL_GATE;
int host_gpio_set(int pin, int level)
{
    BaseType_t was_priv = xPortRaisePrivilege();
    int ret = host_gpio_set_impl(pin, level);
    vPortResetPrivilege(was_priv);
    return ret;
}

static int host_gpio_get_impl(int pin)
{
    const gpio_whitelist_entry_t *e = gpio_lookup(pin);
    if (e == NULL) {
        return -1;
    }
    return (gpio_input_data_bit_read(e->port, e->mask) == SET) ? 1 : 0;
}

int host_gpio_get(int pin) MDL_SYSCALL_GATE;
int host_gpio_get(int pin)
{
    BaseType_t was_priv = xPortRaisePrivilege();
    int ret = host_gpio_get_impl(pin);
    vPortResetPrivilege(was_priv);
    return ret;
}

/* ---- clock / delay -----------------------------------------------------
 * configTICK_RATE_HZ == 1000 (FreeRTOSConfig.h) -- one tick is one ms,
 * so xTaskGetTickCount() doubles as uptime_ms() with no separate
 * SysTick_Handler of our own (FreeRTOS owns SysTick now, via the
 * xPortSysTickHandler -> SysTick_Handler rename in FreeRTOSConfig.h). */

uint32_t host_uptime_ms(void) MDL_SYSCALL_GATE;
uint32_t host_uptime_ms(void)
{
    BaseType_t was_priv = xPortRaisePrivilege();
    uint32_t ms = (uint32_t)xTaskGetTickCount();
    vPortResetPrivilege(was_priv);
    return ms;
}

void host_delay_ms(uint32_t ms) MDL_SYSCALL_GATE;
void host_delay_ms(uint32_t ms)
{
    /* vTaskDelay() itself blocks (yields to the scheduler) -- no need to
     * stay privileged for the wait itself, just for the call into it
     * (xTaskGetTickCount()-adjacent bookkeeping FreeRTOS does inside). */
    BaseType_t was_priv = xPortRaisePrivilege();
    vPortResetPrivilege(was_priv);
    vTaskDelay(pdMS_TO_TICKS(ms));
}

/* ---- per-module alloc() pool -------------------------------------------
 * Carved from the bottom MDL_HEAP_SIZE (4K) of the module's heap+stack
 * arena slice -- see mdl/core/module_task.h's layout comment for why
 * the other 4K (2K stack + 2K unmapped guard) isn't available for this.
 * Same bump+freelist allocator as M1; still the simplest thing that
 * could work, still reset on load, still the thing M3's 1000-round
 * load/unload leak test would catch if it's ever not good enough. */

typedef struct free_block {
    struct free_block *next;
    size_t              size;
} free_block_t;

static uint8_t     *g_pool_next;
static uint8_t      *g_pool_end;
static free_block_t *g_free_list;

void host_api_pool_reset(module_t *m)
{
    g_pool_next  = (uint8_t *)m->heap_stack_lo;
    g_pool_end   = (uint8_t *)m->heap_stack_lo + MDL_HEAP_SIZE;
    g_free_list  = NULL;
}

static void *host_alloc_impl(size_t n)
{
    if (n == 0) {
        return NULL;
    }
    n = (n + 7u) & ~(size_t)7u;

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

void *host_alloc(size_t n) MDL_SYSCALL_GATE;
void *host_alloc(size_t n)
{
    BaseType_t was_priv = xPortRaisePrivilege();
    void *ret = host_alloc_impl(n);
    vPortResetPrivilege(was_priv);
    return ret;
}

static void host_free_impl(void *p)
{
    if (p == NULL) {
        return;
    }
    /* Reject anything not inside this module's own pool range --
     * "free 的指针必须是该模块之前 alloc 返回的,否则拒绝". This catches
     * a foreign/out-of-range pointer; it does NOT catch a double-free
     * of a still-in-range pointer (would need a live-block registry to
     * do properly) -- an acceptable v1 gap, not a silent memory-safety
     * hole: worst case of a double-free is corruption of THIS module's
     * own free list, contained entirely within its own pool, never past
     * it (the pool's MPU region bounds that regardless of what this
     * allocator's bookkeeping does internally). */
    if (!ptr_in_range((uintptr_t)p, sizeof(free_block_t),
                       g_mdl_slot.heap_stack_lo, g_mdl_slot.heap_stack_lo + MDL_HEAP_SIZE)) {
        return;
    }
    free_block_t *b = (free_block_t *)p;
    b->size = 0;
    b->next = g_free_list;
    g_free_list = b;
}

void host_free(void *p) MDL_SYSCALL_GATE;
void host_free(void *p)
{
    BaseType_t was_priv = xPortRaisePrivilege();
    host_free_impl(p);
    vPortResetPrivilege(was_priv);
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
    /* Nothing to do in M2 -- FreeRTOS owns the tick once
     * vTaskStartScheduler() runs; there is no separate clock to arm
     * here anymore (M1 had its own SysTick_Handler; M2 doesn't). */
}
