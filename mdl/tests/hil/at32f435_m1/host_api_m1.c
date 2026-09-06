/*
 * host_api implementation for the M1 target: module runs PRIVILEGED, no
 * RTOS, plain function calls.
 *
 * WHY THIS FILE EXISTS SEPARATELY FROM mdl/host/host_api.c
 *
 * M1's whole point is the milestone host_api.h documents as "M1: modules
 * run privileged and these are plain function calls -- M2 changes only
 * the plumbing, not the semantics or signatures". M2 then rewrote
 * mdl/host/host_api.c into something that cannot be built without an
 * RTOS: every entry point is wrapped in xPortRaisePrivilege() /
 * vPortResetPrivilege() and placed in the `freertos_system_calls` linker
 * section, uptime_ms() is xTaskGetTickCount(), delay_ms() is
 * vTaskDelay(). That is correct for M2+, and it silently broke the M1
 * build (host_api.c gained `#include "FreeRTOS.h"`); nobody noticed
 * because M1 was never rebuilt after M2 landed.
 *
 * So M1 gets its own implementation rather than mdl/host/host_api.c
 * growing #ifdefs for two different privilege models. There is already
 * precedent for exactly this: mdl/tests/qemu/cmsdk_m4/host_api_qemu.c is
 * a second implementation of the same vtable for a different platform.
 * The ABI (host_api.h) is the contract; how a given target satisfies it
 * is that target's business.
 *
 * Behaviour deliberately kept identical to mdl/host/host_api.c where it
 * can be: same pointer validation, same pin whitelist, same bump+freelist
 * pool allocator, same semihosting log sink. The differences are only
 * the ones M1 forces: no SVC gate, no watchdog feed (there is no
 * supervisor task to feed), SysTick instead of the FreeRTOS tick.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "host_api.h"
#include "registry.h"
#include "module_task.h" /* MDL_HEAP_SIZE */
#include "at32f435_437.h"

/* ---- millisecond timebase ------------------------------------------
 * M2+ gets this free from FreeRTOS. M1 has to run its own SysTick.
 * Programmed in host_api_init() from system_core_clock, so it stays
 * correct whether board_clock_init() has taken the part to 288MHz or it
 * is still on the 48MHz HICK reset default. */

static volatile uint32_t s_uptime_ms;

void SysTick_Handler(void); /* overrides the vendor startup's weak stub */
void SysTick_Handler(void)
{
    s_uptime_ms++;
}

/* ---- pointer validation (identical to mdl/host/host_api.c) ---------- */

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

/* ---- pin whitelist (UYUP-RPI-A-2.4, same table as mdl/host/host_api.c) */

typedef struct {
    gpio_type *port;
    uint16_t   mask;
    uint32_t   clock;
    bool       is_output;
} gpio_whitelist_entry_t;

static const gpio_whitelist_entry_t g_gpio_whitelist[] = {
    { GPIOD, GPIO_PINS_10, CRM_GPIOD_PERIPH_CLOCK, true  }, /* 0: LEDB  (PD10) */
    { GPIOE, GPIO_PINS_15, CRM_GPIOE_PERIPH_CLOCK, true  }, /* 1: LEDG  (PE15) */
    { GPIOA, GPIO_PINS_3,  CRM_GPIOA_PERIPH_CLOCK, false }, /* 2: BTN0  (PA3)  */
    { GPIOE, GPIO_PINS_2,  CRM_GPIOE_PERIPH_CLOCK, false }, /* 3: BTN1  (PE2)  */
};
#define GPIO_WHITELIST_COUNT (sizeof(g_gpio_whitelist) / sizeof(g_gpio_whitelist[0]))

static const gpio_whitelist_entry_t *gpio_lookup(int pin)
{
    if (pin < 0 || (size_t)pin >= GPIO_WHITELIST_COUNT) {
        return NULL;
    }
    return &g_gpio_whitelist[pin];
}

static void gpio_whitelist_init(void)
{
    for (size_t i = 0; i < GPIO_WHITELIST_COUNT; i++) {
        const gpio_whitelist_entry_t *e = &g_gpio_whitelist[i];
        gpio_init_type cfg;

        crm_periph_clock_enable(e->clock, TRUE);
        gpio_default_para_init(&cfg);
        cfg.gpio_pins = e->mask;
        if (e->is_output) {
            cfg.gpio_mode           = GPIO_MODE_OUTPUT;
            cfg.gpio_out_type       = GPIO_OUTPUT_PUSH_PULL;
            cfg.gpio_pull           = GPIO_PULL_NONE;
            cfg.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
            gpio_init(e->port, &cfg);
            gpio_bits_write(e->port, e->mask, TRUE); /* active-low LED: off */
        } else {
            cfg.gpio_mode = GPIO_MODE_INPUT;
            cfg.gpio_pull = GPIO_PULL_UP;
            gpio_init(e->port, &cfg);
        }
    }
}

/* ---- log: ARM semihosting SYS_WRITE0 --------------------------------
 * Ozone picks this up in its Terminal window, which is the only output
 * channel M1 has (no USB CDC until M4, no UART wired up here). See
 * mdl/host/host_api.c for why this helper must not be inlined. */

__attribute__((noinline))
static void semihost_write0(const char *msg)
{
    register uint32_t r0 __asm__("r0") = 0x04;
    register const char *r1 __asm__("r1") = msg;
    __asm__ volatile("bkpt 0xAB" : : "r"(r0), "r"(r1) : "memory");
}

#define LOG_MAX_LEN 127

static void host_log(const char *msg)
{
    if (!ptr_owned_by_module(msg, 1)) {
        semihost_write0("[host] log() rejected: msg not in module's own memory");
        return;
    }
    char buf[LOG_MAX_LEN + 1];
    size_t i = 0;
    while (i < LOG_MAX_LEN && ptr_owned_by_module(msg + i, 1) && msg[i] != '\0') {
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

/* ---- clock / delay --------------------------------------------------- */

static uint32_t host_uptime_ms(void)
{
    return s_uptime_ms;
}

static void host_delay_ms(uint32_t ms)
{
    /* M1 has no scheduler to yield to, so this is a real busy-wait --
     * host_api.h documents delay_ms() as cooperative, which it becomes
     * from M2 on (vTaskDelay). The observable contract ("blocks for at
     * least ms milliseconds") holds either way; only the cost to the
     * rest of the system differs, and in M1 there is no rest of the
     * system. Subtraction, not >=, so a wrap of s_uptime_ms is safe. */
    uint32_t start = s_uptime_ms;
    while ((s_uptime_ms - start) < ms) {
    }
}

/* ---- per-module alloc() pool (identical to mdl/host/host_api.c) ------ */

typedef struct free_block {
    struct free_block *next;
    size_t              size;
} free_block_t;

static uint8_t      *g_pool_next;
static uint8_t      *g_pool_end;
static free_block_t *g_free_list;

void host_api_pool_reset(module_t *m)
{
    g_pool_next = (uint8_t *)m->heap_stack_lo;
    g_pool_end  = (uint8_t *)m->heap_stack_lo + MDL_HEAP_SIZE;
    g_free_list = NULL;
}

static void *host_alloc(size_t n)
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

static void host_free(void *p)
{
    if (p == NULL) {
        return;
    }
    if (!ptr_in_range((uintptr_t)p, sizeof(free_block_t),
                       g_mdl_slot.heap_stack_lo,
                       g_mdl_slot.heap_stack_lo + MDL_HEAP_SIZE)) {
        return;
    }
    free_block_t *b = (free_block_t *)p;
    b->size = 0;
    b->next = g_free_list;
    g_free_list = b;
}

/* ---- vtable ---------------------------------------------------------- */

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
    gpio_whitelist_init();

    /* 1kHz SysTick from whatever sclk board_clock_init() settled on.
     * system_core_clock is only correct after system_core_clock_update(),
     * which board_clock_init() calls -- so main() must call that first. */
    SysTick_Config(system_core_clock / 1000u);
}
