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

/*
 * Record that the MDL is alive [ABI v3 semantics -- see registry.h].
 *
 * Called from exactly two places now, not from every gate: the explicit
 * watchdog_feed(), and the far side of delay_ms(). Feeding on any host
 * call made the watchdog unable to detect the thing it exists for --
 * `while (1) { host->uptime_ms(); }` fed it forever.
 */
static void feed_watchdog(void)
{
    g_mdl_slot.last_active_tick = (uint32_t)xTaskGetTickCount();
}

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
    uint32_t   clock;
    bool       is_output;
    /* false = listed so it can be REFUSED by name, never configured.
     * PA9/PA10 are the DAP debug UART; calling gpio_init() on them takes
     * the console away, which is precisely the outcome the entry exists
     * to prevent. */
    bool       configure;

    /* EXINT wiring, for pins an MDL may take interrupts on [ABI v4].
     * exti_irqn < 0 means this pin cannot raise one -- outputs, and any
     * input on lines 5..15, which share a single vector and would need a
     * demultiplexing handler rather than a dedicated one. */
    uint8_t    exti_port_source;
    uint8_t    exti_pin_source;
    uint32_t   exti_line;
    int16_t    exti_irqn;

    /* The physical pin, in the port<<4|num space conflict detection uses.
     * Without it a GPIO claim could not be compared against a peripheral
     * claim at all -- they would be numbers from two different spaces. */
    uint8_t    pin_id;
} gpio_whitelist_entry_t;

/*
 * The UYUP-RPI-A-2.4 board's actual pins (schematic UYUP-RPI-A-2.4.pdf),
 * replacing the PA0/PA1 placeholder this table shipped with. A module
 * sees these as pin indices 0..3 and cannot reach anything else --
 * host_api.h's gpio_set()/gpio_get() contract is unchanged, only what
 * the indices mean.
 *
 * LED polarity: both LEDs sit between VDD and their pin through a 1k
 * resistor, i.e. they are almost certainly active-LOW -- gpio_set(0, 0)
 * lights LEDB. Read "almost certainly" literally: this was inferred from
 * the schematic netlist, not measured. If the LED turns out inverted on
 * the bench, that is a fact about the board, not a bug in this table.
 *
 * Silkscreen mapping, read off the schematic's LED+BUTTON+INTERFACE
 * sheet rather than guessed: SW3 is PA3_BTN0 and SW4 is PE2_BTN1; LED3
 * is PD10_LEDB (blue) and LED4 is PE15_LEDG (green). Both LEDs sit with
 * their cathode on the MCU pin and a 1k resistor up to VDD, so the pin
 * SINKS the current and driving it LOW is what lights them.
 *
 * Buttons pull to GND when pressed (10k pull-ups R83/R84), so they are
 * configured with the internal pull-up and read 1 when idle, 0 pressed.
 */
static const gpio_whitelist_entry_t g_gpio_whitelist[] = {
    { GPIOD, GPIO_PINS_10, CRM_GPIOD_PERIPH_CLOCK, true,  true,
      0, 0, 0, -1, MDL_PIN(3, 10) },  /* 0: LEDB = LED3 blue  (PD10) */
    { GPIOE, GPIO_PINS_15, CRM_GPIOE_PERIPH_CLOCK, true,  true,
      0, 0, 0, -1, MDL_PIN(4, 15) },  /* 1: LEDG = LED4 green (PE15) */
    { GPIOA, GPIO_PINS_3,  CRM_GPIOA_PERIPH_CLOCK, false, true,
      SCFG_PORT_SOURCE_GPIOA, SCFG_PINS_SOURCE3, EXINT_LINE_3, EXINT3_IRQn,
      MDL_PIN(0, 3) },
                      /* 2: BTN0 = SW3 (PA3) -- exint line 3, own vector */
    { GPIOE, GPIO_PINS_2,  CRM_GPIOE_PERIPH_CLOCK, false, true,
      SCFG_PORT_SOURCE_GPIOE, SCFG_PINS_SOURCE2, EXINT_LINE_2, EXINT2_IRQn,
      MDL_PIN(4, 2) },
                      /* 3: BTN1 = SW4 (PE2) -- exint line 2, own vector */
    { GPIOA, GPIO_PINS_9,  CRM_GPIOA_PERIPH_CLOCK, false, false,
      0, 0, 0, -1, MDL_PIN(0, 9) },  /* 4: U1TX (PA9) -- listed to be refused */
};

static const char *const g_gpio_names[] = {
    "LEDB (PD10, LED3 blue,  out, 0=lit)",
    "LEDG (PE15, LED4 green, out, 0=lit)",
    "BTN0 (PA3,  SW3, in,  1=idle)",
    "BTN1 (PE2,  SW4, in,  1=idle)",
    "U1TX (PA9,  DAP debug UART -- never a module's to take)",
};
#define GPIO_WHITELIST_COUNT (sizeof(g_gpio_whitelist) / sizeof(g_gpio_whitelist[0]))

/*
 * Pins the HOST keeps for itself, and why -- NULL means a module may
 * claim it.
 *
 * Pin 0 is the case this whole mechanism exists for. main.c's
 * indicator_task blinks LEDB once a second as the 'firmware is alive'
 * signal, which is the one thing you look at to decide whether the board
 * is running at all when USB is dead. A module driving the same pin
 * crashes nothing; it just makes that signal meaningless, and a
 * meaningless liveness indicator is worse than none, because it is
 * still believed. Two owners with no arbitration is the bug, not the
 * flicker it produces.
 */
/*
 * How firmly the host holds each pin.
 *
 * HARD is a refusal: the pin does something the host cannot stop doing
 * and still be a working host.
 *
 * YIELD is the interesting one, and it is what LEDB is. The host blinks
 * it once a second as its 'firmware alive' signal, so by default it is
 * in use -- but that is a courtesy, not a requirement, and a module that
 * explicitly declares the pin gets it. What must not happen is the
 * UNDECLARED case: two writers, no arbitration, and a liveness indicator
 * that has quietly stopped meaning anything while still being believed.
 * Declaring it is what turns a collision into a handover.
 */
typedef enum {
    GPIO_FREE = 0,
    GPIO_HOST_YIELDS,
    GPIO_HOST_HARD,
} gpio_ownership_t;

typedef struct {
    gpio_ownership_t how;
    const char      *who;
} gpio_owner_t;

static const gpio_owner_t g_gpio_owner[GPIO_WHITELIST_COUNT] = {
    { GPIO_HOST_YIELDS, "host alive-blink (yields if you declare it)" }, /* 0: LEDB */
    { GPIO_HOST_YIELDS, "host USB-link LED (yields if you declare it)" }, /* 1: LEDG */
    { GPIO_FREE,        NULL },                                           /* 2: BTN0 */
    { GPIO_FREE,        NULL },                                           /* 3: BTN1 */
    { GPIO_HOST_HARD,   "the DAP debug UART (PA9/PA10)" },                /* 4: U1TX */
};

/*
 * True while a module has declared this pin, so the host's own indicator
 * task can stand down instead of fighting it. Checked by main.c.
 */
bool host_gpio_yielded_to_module(int pin)
{
    if (pin < 0 || (size_t)pin >= GPIO_WHITELIST_COUNT) {
        return false;
    }
    if (g_gpio_owner[pin].how != GPIO_HOST_YIELDS) {
        return false;
    }
    /* An empty slot cannot be holding anything, whatever gpio_claimed
     * still says. reclaim_module() clears it now, but this is the check
     * that makes forgetting to harmless rather than a dark LED. */
    if (g_mdl_slot.state == MDL_SLOT_EMPTY) {
        return false;
    }
    return (g_mdl_slot.gpio_claimed & (1u << (unsigned)pin)) != 0u;
}

/*
 * Strong override of loader.c's weak hook: answers 'is this already
 * spoken for' at LOAD time, before any of the image is copied in.
 */
const char *mdl_res_owner(uint8_t kind, uint8_t id)
{
    if (kind != (uint8_t)MDL_RES_KIND_GPIO) {
        return "an unknown resource class";
    }
    if (id >= GPIO_WHITELIST_COUNT) {
        return "nothing -- that pin is not on the whitelist at all";
    }
    /* Only a HARD claim refuses the load. A YIELDS pin is granted, and
     * the host stops driving it -- see host_gpio_yielded_to_module(). */
    return (g_gpio_owner[id].how == GPIO_HOST_HARD) ? g_gpio_owner[id].who : NULL;
}

static const gpio_whitelist_entry_t *gpio_lookup(int pin);

/* Board knowledge for the event layer: which EXINT line and vector a
 * whitelist pin sits on. host_events.c owns the queue, this owns the
 * wiring, so neither has to know the other's business. */
bool host_gpio_exti_info(int pin, uint8_t *port_source, uint8_t *pin_source,
                          uint32_t *line, int *irqn)
{
    const gpio_whitelist_entry_t *e = gpio_lookup(pin);
    if (e == NULL || !e->configure || e->exti_irqn < 0) {
        return false;
    }
    *port_source = e->exti_port_source;
    *pin_source  = e->exti_pin_source;
    *line        = e->exti_line;
    *irqn        = (int)e->exti_irqn;
    return true;
}

/* The physical pin behind a whitelist index, for host_resources.c's
 * pin-level comparison. False for an index that is not whitelisted. */
bool host_gpio_pin_id(int pin, uint8_t *out)
{
    const gpio_whitelist_entry_t *e = gpio_lookup(pin);
    if (e == NULL) {
        return false;
    }
    *out = e->pin_id;
    return true;
}

const char *host_gpio_host_owner(int pin)
{
    if (pin < 0 || (unsigned)pin >= GPIO_WHITELIST_COUNT) {
        return NULL;
    }
    return g_gpio_owner[pin].who;
}

/*
 * The second half of the two-stage check (mdl_format.h's mdl_res_t
 * comment explains why one stage is not enough): the loader already
 * refused conflicting CLAIMS, this refuses USE of anything the module
 * did not claim. Without it a module could declare pin 1 and drive pin
 * 0 anyway, and the manifest would be a comment rather than a rule.
 */
static bool module_claimed(int pin)
{
    if (pin < 0 || pin >= 32) {
        return false;
    }
    return (g_mdl_slot.gpio_claimed & (1u << (unsigned)pin)) != 0u;
}

/*
 * Configure every whitelisted pin. Until this existed, host_api_init()
 * was an empty function and nothing ever put these pins into output or
 * input mode -- gpio_set() wrote to a pin still in its reset state and
 * changed nothing observable. Called from host_api_init(), privileged,
 * once at boot.
 */
/*
 * Put one pin into its defined default state.
 *
 * Deliberately the ONLY definition of what 'default' means for a pin, so
 * boot and release cannot drift apart. Release matters as much as boot:
 * whatever an MDL left a pin doing outlives the MDL unless something
 * actively undoes it, and the next MDL would inherit a pin in a state it
 * never asked for and cannot see.
 */
static void gpio_set_default(const gpio_whitelist_entry_t *e)
{
    if (!e->configure) {
        return; /* listed to be refused, not to be driven */
    }

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
        /* Active-low LED: drive high = off, so a pin nobody is using is
         * dark rather than lit. */
        gpio_bits_write(e->port, e->mask, TRUE);
    } else {
        cfg.gpio_mode = GPIO_MODE_INPUT;
        cfg.gpio_pull = GPIO_PULL_UP;
        gpio_init(e->port, &cfg);
    }
}

static void gpio_whitelist_init(void)
{
    for (size_t i = 0; i < GPIO_WHITELIST_COUNT; i++) {
        gpio_set_default(&g_gpio_whitelist[i]);
    }
}

/*
 * Hand every pin this MDL declared back to the host, in a defined state.
 *
 * Called from reclaim_module() BEFORE gpio_claimed is cleared -- after,
 * there would be no record of what to restore.
 *
 * Today an MDL can only change a pin's LEVEL (it reaches hardware solely
 * through the vtable, and has no MPU region over any peripheral), so in
 * practice this puts LEDs out. It re-applies the full configuration
 * rather than just the level because that stops being enough the moment
 * MDL_RES_KIND_* grows I2C/UART/ADC and a claim starts implying a pin
 * mux -- and a half-released pin is the kind of residue that surfaces
 * three MDLs later as inexplicable behaviour.
 */
uint32_t host_gpio_release_claims(uint32_t claimed)
{
    uint32_t restored = 0;
    for (size_t i = 0; i < GPIO_WHITELIST_COUNT; i++) {
        if ((claimed & (1u << (unsigned)i)) != 0u) {
            gpio_set_default(&g_gpio_whitelist[i]);
            restored |= (1u << (unsigned)i);
        }
    }
    /* Returned so the caller can SAY which pins went back. On this board
     * both LEDs are also host indicators, so the host relights them
     * immediately and the restore is electrically invisible from outside
     * -- indistinguishable from never having happened. Reporting it is
     * the only way to tell the two apart. */
    return restored;
}
static const gpio_whitelist_entry_t *gpio_lookup(int pin)
{
    if (pin < 0 || (size_t)pin >= GPIO_WHITELIST_COUNT) {
        return NULL;
    }
    return &g_gpio_whitelist[pin];
}

/* ---- log: ARM semihosting SYS_WRITE0 (placeholder until M4's USB CDC) */

/* noinline: cheap insurance against a real, reproducible bug found
 * while bringing up the QEMU target (mdl/tests/qemu/cmsdk_m4): at -O1+
 * GCC inlines this at every call site, and two inlined calls
 * back-to-back in the same caller (this file's host_log_impl() does
 * exactly that) produce adjacent `bkpt 0xAB` instructions that QEMU's
 * semihosting emulation mishandles (second call traps with garbage in
 * r0). Real hardware/debuggers don't have this problem, and this file
 * doesn't run under QEMU -- but the fix is free, so it stays here too
 * rather than only in the QEMU-specific copy. */
__attribute__((noinline))
static void semihost_write0(const char *msg)
{
    register uint32_t r0 __asm__("r0") = 0x04;
    register const char *r1 __asm__("r1") = msg;
    __asm__ volatile("bkpt 0xAB" : : "r"(r0), "r"(r1) : "memory");
}

/*
 * Where host->log() text actually goes.
 *
 * Weak default: ARM semihosting, and ONLY with a debugger attached.
 * That guard is the whole point. `bkpt 0xAB` with DHCSR.C_DEBUGEN clear
 * is not a no-op that quietly fails -- it raises a debug event which,
 * with no debug monitor enabled, escalates straight to HardFault
 * (HFSR.DEBUGEVT set, CFSR all zero, which reads like nothing at all
 * went wrong). The first module that called log() on a standalone board
 * therefore killed it, and left a fault record naming a HardFault with
 * no fault-status bits to explain it.
 *
 * A build with a text transport overrides this -- mdl/transport/console.c
 * sends it out the USB console instead, which is what the 'placeholder
 * until M4's USB CDC' note above was always waiting for.
 */
__attribute__((weak)) void host_log_sink(const char *msg)
{
    if (CoreDebug->DHCSR & CoreDebug_DHCSR_C_DEBUGEN_Msk) {
        semihost_write0(msg);
        semihost_write0("\n");
    }
}

#define LOG_MAX_LEN 127

static void host_log_impl(const char *msg)
{
    if (!ptr_owned_by_module(msg, 1)) {
        host_log_sink("[host] log() rejected: msg not in module's own memory");
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
    host_log_sink(buf);
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
    if (e == NULL || !e->configure) {
        return -1; /* unknown, or listed only so it can be refused */
    }

    gpio_bits_write(e->port, e->mask, level ? TRUE : FALSE);
    return 0;
}

int host_gpio_set(int pin, int level) MDL_SYSCALL_GATE;
int host_gpio_set(int pin, int level)
{
    BaseType_t was_priv = xPortRaisePrivilege();
    /* Deliberately here and NOT in host_gpio_set_impl(): the claim is a
     * constraint on MODULES, not a property of the pin. Putting it in the
     * shared impl also gated host_gpio_direct_set(), so with no module
     * loaded the board's own alive-blink and USB-link LEDs went dark --
     * the host refusing itself permission to use its own hardware. */
    int ret = module_claimed(pin) ? host_gpio_set_impl(pin, level) : -1;
    vPortResetPrivilege(was_priv);
    return ret;
}

static int host_gpio_get_impl(int pin)
{
    const gpio_whitelist_entry_t *e = gpio_lookup(pin);
    if (e == NULL || !e->configure) {
        return -1;
    }

    return (gpio_input_data_bit_read(e->port, e->mask) == SET) ? 1 : 0;
}

int host_gpio_get(int pin) MDL_SYSCALL_GATE;
int host_gpio_get(int pin)
{
    BaseType_t was_priv = xPortRaisePrivilege();
    /* Reading is a claim too -- it says this pin is yours to observe.
     * Same placement reasoning as host_gpio_set(). */
    int ret = module_claimed(pin) ? host_gpio_get_impl(pin) : -1;
    vPortResetPrivilege(was_priv);
    return ret;
}

/* Host-side, already-privileged, deliberately NOT watchdog-feeding --
 * see host_api.h for why that distinction is the whole point. */
int host_gpio_direct_set(int pin, int level)
{
    return host_gpio_set_impl(pin, level);
}

int host_gpio_direct_get(int pin)
{
    return host_gpio_get_impl(pin);
}

const char *host_gpio_name(int pin)
{
    if (pin < 0 || (size_t)pin >= GPIO_WHITELIST_COUNT) {
        return NULL;
    }
    return g_gpio_names[pin];
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
    /* Park across the wait. An MDL blocked inside the host is not hung,
     * and without this the watchdog would kill anything that sleeps for
     * longer than its timeout -- which is most resident MDLs. */
    BaseType_t was_priv = xPortRaisePrivilege();
    g_mdl_slot.parked_until_tick = (uint32_t)xTaskGetTickCount() + ms + 1u;
    vPortResetPrivilege(was_priv);

    /* vTaskDelay() blocks; no reason to hold privilege across it. */
    vTaskDelay(pdMS_TO_TICKS(ms));

    was_priv = xPortRaisePrivilege();
    g_mdl_slot.parked_until_tick = 0;
    feed_watchdog(); /* came back from the wait -- that IS progress */
    vPortResetPrivilege(was_priv);
}

void host_watchdog_feed(void) MDL_SYSCALL_GATE;
void host_watchdog_feed(void)
{
    BaseType_t was_priv = xPortRaisePrivilege();
    feed_watchdog();
    vPortResetPrivilege(was_priv);
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
    /* The first MDL_ARGBLOCK_SIZE bytes of the heap region are the
     * console-argument block (module_task.h), written by the host just
     * before each module_cmd() call. Handing them out via alloc() too
     * would let a module's own allocation be overwritten by the next
     * command's argv. */
    g_pool_next  = (uint8_t *)m->heap_stack_lo + MDL_ARGBLOCK_SIZE;
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

/*
 * Decimal parse for module arguments [ABI v2]. Modules build -nostdlib,
 * so without this every module taking a console argument reimplements
 * it. Saturates instead of wrapping: a module asked to blink
 * 99999999999 times should get INT_MAX, not a small negative number.
 */
static int host_atoi_impl(const char *s)
{
    if (!ptr_owned_by_module(s, 1)) {
        return 0;
    }
    int sign = 1;
    size_t i = 0;
    if (ptr_owned_by_module(s, 1) && (s[0] == '-' || s[0] == '+')) {
        sign = (s[0] == '-') ? -1 : 1;
        i = 1;
    }
    long v = 0;
    while (ptr_owned_by_module(s + i, 1) && s[i] >= '0' && s[i] <= '9') {
        v = v * 10 + (s[i] - '0');
        if (v > 2147483647L) {
            v = 2147483647L;
            break;
        }
        i++;
    }
    return (int)(sign * (int)v);
}

int host_atoi(const char *s) MDL_SYSCALL_GATE;
int host_atoi(const char *s)
{
    BaseType_t was_priv = xPortRaisePrivilege();
    int ret = host_atoi_impl(s);
    vPortResetPrivilege(was_priv);
    return ret;
}

/*
 * Cycle counter [ABI v6].
 *
 * DWT CYCCNT is free running once enabled and costs one load to read, so
 * it is the only clock fine enough to see what this project claims about
 * itself: at 288MHz a tick is ~3.5ns, while the RTOS tick is 1ms and
 * would round every interesting number to zero.
 *
 * TRCENA is set by software here rather than left to a debugger, so the
 * numbers are the same whether or not one is attached -- a benchmark that
 * only works under a debugger measures the debugger.
 */
static void cycles_init(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

uint32_t host_cycles_now(void)
{
    return DWT->CYCCNT;
}

/* Gated, like every other vtable entry -- which is exactly why timing a
 * single host call with it is meaningless and a loop is required: the
 * measurement pays the same SVC round trip as the thing measured. */
uint32_t host_cycles(void) MDL_SYSCALL_GATE;
uint32_t host_cycles(void)
{
    BaseType_t was_priv = xPortRaisePrivilege();
    uint32_t c = DWT->CYCCNT;
    vPortResetPrivilege(was_priv);
    return c;
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
    .atoi      = host_atoi,
    .watchdog_feed = host_watchdog_feed,
    .cycles    = host_cycles,
};

void host_api_init(void)
{
    /* No clock to arm: FreeRTOS owns the tick once vTaskStartScheduler()
     * runs (M1 had its own SysTick_Handler; M2 onward doesn't). The pins
     * the vtable exposes do still need configuring, though. */
    cycles_init();
    gpio_whitelist_init();
}
