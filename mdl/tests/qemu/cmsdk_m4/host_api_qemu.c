/*
 * QEMU target's host_api.c -- same SVC-gate mechanism as
 * mdl/host/host_api.c (M2's real AT32 implementation), but with GPIO
 * stubbed out (QEMU's mps2-an386 model has no board GPIO this project
 * wires up) instead of calling into AT32's gpio driver. Kept as a
 * separate file rather than #ifdef-ing the real one -- the GPIO
 * functions are the only genuinely AT32-specific part, and duplicating
 * ~15 lines is clearer than threading a target #ifdef through the
 * shared file.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "host_api.h"
#include "registry.h"
#include "module_task.h"
#include "FreeRTOS.h"
#include "task.h"

extern BaseType_t xPortRaisePrivilege(void);
extern void vPortResetPrivilege(BaseType_t xRunningPrivileged);

#define MDL_SYSCALL_GATE __attribute__((section("freertos_system_calls")))

static void feed_watchdog(void)
{
    g_mdl_slot.last_active_tick = (uint32_t)xTaskGetTickCount();
}

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
        return true;
    }
    module_t *m = &g_mdl_slot;
    return ptr_in_range(addr, len, m->text_lo, m->text_hi) ||
           ptr_in_range(addr, len, m->data_lo, m->data_hi) ||
           ptr_in_range(addr, len, m->heap_stack_lo, m->heap_stack_lo + MDL_HEAP_SIZE);
}

/*
 * noinline: NOT optional under QEMU. At -O1+, GCC inlines this short
 * leaf function at every call site; when two calls happen back-to-back
 * in the same caller (e.g. host_log_impl()'s message + "\n" below), the
 * inlined result is two `bkpt 0xAB` instructions with nothing between
 * them. QEMU's semihosting emulation (verified empirically in this
 * session -- a real, reproducible QEMU bug/quirk, not a guess) mishandles
 * that adjacency: the SECOND bkpt traps with garbage in r0 (observed:
 * 0xdeadbeef) instead of the SYS_WRITE0 value this function just set,
 * and QEMU reports "Unsupported SemiHosting SWI 0xdeadbeef" and halts.
 * Forcing a real bl/bx call around each invocation (this attribute)
 * guarantees real instructions between consecutive bkpts and avoids it
 * entirely. Real hardware/debuggers do not have this problem -- it is
 * specific to this QEMU semihosting emulation, but the fix costs
 * nothing to keep even where it doesn't matter.
 */
__attribute__((noinline))
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
        semihost_write0("[host] log() rejected: msg not in module's own memory\n");
        return;
    }
    char buf[LOG_MAX_LEN + 2];
    size_t i = 0;
    while (i < LOG_MAX_LEN && ptr_owned_by_module(msg + i, 1) && msg[i] != '\0') {
        buf[i] = msg[i];
        i++;
    }
    buf[i] = '\n';
    buf[i + 1] = '\0';
    semihost_write0(buf);
}

void host_log(const char *msg) MDL_SYSCALL_GATE;
void host_log(const char *msg)
{
    BaseType_t was_priv = xPortRaisePrivilege();
    feed_watchdog();
    host_log_impl(msg);
    vPortResetPrivilege(was_priv);
}

/* No GPIO wired up on this target -- every pin is "not whitelisted",
 * same failure mode a real AT32 build gives for an out-of-range pin
 * number, so module code that checks gpio_set()'s return value behaves
 * identically either way. */
int host_gpio_set(int pin, int level) MDL_SYSCALL_GATE;
int host_gpio_set(int pin, int level)
{
    (void)pin;
    (void)level;
    BaseType_t was_priv = xPortRaisePrivilege();
    feed_watchdog();
    vPortResetPrivilege(was_priv);
    return -1;
}

int host_gpio_get(int pin) MDL_SYSCALL_GATE;
int host_gpio_get(int pin)
{
    (void)pin;
    BaseType_t was_priv = xPortRaisePrivilege();
    feed_watchdog();
    vPortResetPrivilege(was_priv);
    return -1;
}

uint32_t host_uptime_ms(void) MDL_SYSCALL_GATE;
uint32_t host_uptime_ms(void)
{
    BaseType_t was_priv = xPortRaisePrivilege();
    feed_watchdog();
    uint32_t ms = (uint32_t)xTaskGetTickCount();
    vPortResetPrivilege(was_priv);
    return ms;
}

void host_delay_ms(uint32_t ms) MDL_SYSCALL_GATE;
void host_delay_ms(uint32_t ms)
{
    BaseType_t was_priv = xPortRaisePrivilege();
    feed_watchdog();
    vPortResetPrivilege(was_priv);
    vTaskDelay(pdMS_TO_TICKS(ms));
}

typedef struct free_block {
    struct free_block *next;
    size_t              size;
} free_block_t;

static uint8_t     *g_pool_next;
static uint8_t      *g_pool_end;
static free_block_t *g_free_list;

void host_api_pool_reset(module_t *m)
{
    g_pool_next = (uint8_t *)m->heap_stack_lo;
    g_pool_end  = (uint8_t *)m->heap_stack_lo + MDL_HEAP_SIZE;
    g_free_list = NULL;
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
    feed_watchdog();
    void *ret = host_alloc_impl(n);
    vPortResetPrivilege(was_priv);
    return ret;
}

static void host_free_impl(void *p)
{
    if (p == NULL) {
        return;
    }
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
    feed_watchdog();
    host_free_impl(p);
    vPortResetPrivilege(was_priv);
}

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
}

/* Public, non-gated -- main.c/fault_arm.c-adjacent code on this target
 * uses this directly for boot-time/fault-time logging where going
 * through the SVC gate machinery isn't necessary (privileged context
 * already). Not part of host_api_t -- modules never see this symbol. */
void qemu_log(const char *msg)
{
    semihost_write0(msg);
}

/*
 * mdl/core/supervisor.c links against mdl/transport/protocol.c
 * unconditionally (that dependency was introduced in M4 and this QEMU
 * target reuses supervisor.c as-is rather than forking it) -- but this
 * target has no actual transport (no USB, no serial), so
 * mdl_proto_take_frame() never has a real frame to hand back and this
 * is simply never called in practice. A stub, not a real transport.
 */
void mdl_transport_write(const uint8_t *data, uint32_t len)
{
    (void)data;
    (void)len;
}
