#ifndef MDL_HOST_API_H
#define MDL_HOST_API_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
/* The wire format is genuinely shared: MDL_MODULE_RESOURCES() below emits
 * mdl_res_t records that the packer copies verbatim into the .mdl and the
 * loader reads back, so the module, the packer and the firmware must all
 * agree on that struct. Modules therefore compile with mdl/core on the
 * include path (tools/watch.py and tools/mock_host/run.py both set it). */
#include "mdl_format.h"

/*
 * host_api.h -- THE capability boundary for modules.
 *
 * This header is the only thing a module author (human or agent) needs
 * to read to know what a module can do. A module never resolves host
 * symbols by name and never links against host code directly -- the only
 * way in is the host_api_t pointer handed to module_init(). If a
 * capability isn't listed here, the module cannot do it, full stop --
 * there is no escape hatch, no "just call the driver directly."
 *
 * ABI STABILITY: host_api_t is a versioned, append-only struct. abi_ver
 * is checked by the packer at pack time and by the loader at load time;
 * a module built against a different abi_ver than the running host is
 * rejected before it ever executes. Existing fields never change type,
 * meaning, or position -- new capabilities are added at the end, behind
 * a version bump.
 *
 * CALLING CONVENTION (from M2 onward): every function here is an SVC
 * trampoline. Calling it costs a full privilege-escalation round trip
 * (svc -> handler -> validate args -> do the thing -> return), not a
 * plain function call. Do not call these in a tight loop expecting
 * plain-call latency. (M1: modules run privileged and these are plain
 * function calls -- still correct to code against this contract, since
 * M2 changes only the plumbing, not the semantics or signatures below.)
 *
 * THREAD CONTEXT: every function below is documented for whether it is
 * safe to call from module_init() (loader task context, still starting
 * up), from the module's own task body (normal running context), and
 * from a host-invoked module callback (e.g. a registered timer/IRQ
 * bottom-half -- module callbacks always run in the module's own task
 * context via a host-side dispatch, never in interrupt context; there is
 * currently no callback-registration API in this header, so this
 * distinction is forward-looking).
 */

/* v8 (2026-09-08): uart_config/write/read. Unlike I2C, a UART is a
 * stream: data arrives whether or not anyone is reading, so the host
 * buffers it and MDL_EVT_UART says when there is some.
 *
 * v7 (2026-09-08): i2c_write/read/write_read -- the first peripheral an
 * MDL can actually drive, rather than only claim. Requires the matching
 * MDL_RES_I2C(n) declaration; the bus is checked on every call.
 *
 * v6 (2026-09-08): cycles(), and a cycle stamp on every event, so an MDL
 * can measure the sandbox's own overhead from inside itself instead of
 * the claim resting on assertion.
 *
 * v5 (2026-09-07): peripheral claims (I2C/UART/TIMER) and pin-level
 * conflict detection. Claiming only -- no peripheral operations in the
 * vtable yet.
 *
 * v4 (2026-09-07): events. module_event() + MDL_MODULE_EVENTS() +
 * MDL_RES_GPIO_IRQ(). The module task became resident and single: it
 * drains one queue that carries both hardware events and console
 * commands, because an MDL cannot simultaneously block waiting for an
 * event and be restarted to run a command.
 *
 * v3 (2026-09-07): added watchdog_feed(), and CHANGED WHAT FEEDS THE
 * WATCHDOG -- an ordinary host call no longer counts as a sign of life.
 * See registry.h's last_active_tick. Append-only in layout; the
 * behavioural change is why the version moves.
 *
 * v2 (2026-09-07): added atoi() to the vtable, and the two declaration
 * macros below -- console commands and hardware claims. Append-only:
 * every v1 entry keeps its slot and its meaning. */
#define HOST_API_ABI_VERSION 8

/*
 * Every module source file must invoke this exactly once at file scope.
 * It exports a small const symbol (__mdl_abi_ver) that tools/packer.py
 * reads directly out of the compiled .so and compares against ITS OWN
 * notion of HOST_API_ABI_VERSION (parsed from this same header) --
 * catching "module was compiled against a stale copy of host_api.h" as
 * a pack-time error instead of a load-time mystery:
 *   abi_ver mismatch: module built against v2, host expects v3
 * Without this, a version drift between the header a module happened to
 * be compiled with and the header the packer/firmware were built with
 * would go undetected until (or past) runtime.
 */
#define MDL_MODULE_ABI_DECLARE() \
    __attribute__((used)) const uint32_t __mdl_abi_ver = HOST_API_ABI_VERSION

/*
 * Override the MDL's displayed name [ABI v2, optional].
 *
 * Only needed when the source directory is not what you want `status` to
 * show -- the packer defaults to the directory name, which is right
 * almost always.
 */
#define MDL_MODULE_NAME(n) \
    __attribute__((used, section(".mdl_name"))) \
    const char __mdl_name[MDL_NAME_MAX] = n

/*
 * Declare a console command this module answers to [ABI v2].
 *
 *     MDL_MODULE_COMMAND("blink");
 *     int module_cmd(const host_api_t *host, int argc, const char *const *argv)
 *     { ... }
 *
 * After the module loads, `blink` shows up in the console's own `help`
 * and typing it runs module_cmd(); after `unload` it is gone again.
 * argv[0] is the command name, so argv/argc read exactly like main().
 *
 * WHERE THIS RUNS, and why it is not just a callback: the console lives
 * in the supervisor task, which is PRIVILEGED. Calling into module code
 * from there would execute it privileged and the sandbox would be worth
 * nothing. So the host instead restarts the module's own unprivileged
 * task at module_cmd() and waits for it to finish. Consequences worth
 * knowing:
 *   - globals persist between invocations (they live in the module's
 *     data region, which is not touched between calls);
 *   - locals do not -- every invocation gets a fresh stack;
 *   - argv strings are copied into the module's own memory first (the
 *     console's line buffer is host memory the module cannot read);
 *   - the module cannot start work of its own accord. Timers and
 *     interrupt callbacks need the event-loop lifecycle, which is a
 *     separate piece of work (maintain.md F1).
 *
 * Note the deliberate absence of `static`. Modules link with
 * --gc-sections and nothing in the module references this array, so as
 * a static it is dropped by the LINKER (`used` only binds the
 * compiler) and the packer then sees a module that declares nothing --
 * which it did, silently, the first time. External linkage puts it in
 * the shared object's dynamic symbol table, which is a GC root; that is
 * the same reason __mdl_abi_ver above survives. `retain` would be the
 * tidier fix but this binutils ignores it ('retain attribute ignored').
 */
#define MDL_MODULE_COMMAND(name) \
    __attribute__((used, section(".mdl_command"))) \
    const char __mdl_command[MDL_CMD_NAME_MAX] = name

/*
 * Declare every piece of hardware this module touches [ABI v2].
 *
 *     MDL_MODULE_RESOURCES(MDL_RES_GPIO(1));
 *
 * A module that declares nothing gets nothing: gpio_set()/gpio_get()
 * refuse every pin. Claiming something the host already owns fails the
 * LOAD, with the owner named in the error, before any of the image is
 * copied into the arena.
 */
#define MDL_RES_GPIO(pin) { MDL_RES_KIND_GPIO, (uint8_t)(pin), MDL_EDGE_NONE, 0 }

/* Same claim, but also arm the pin's interrupt [ABI v4]. Edge is one of
 * MDL_EDGE_RISING / FALLING / BOTH. Requires module_event(); the packer
 * refuses an MDL that asks for an edge and has nowhere to deliver it. */
#define MDL_RES_GPIO_IRQ(pin, edge) { MDL_RES_KIND_GPIO, (uint8_t)(pin), (edge), 0 }

/*
 * Peripheral claims [ABI v5]. The instance number is the one in the
 * datasheet: MDL_RES_I2C(1) is I2C1.
 *
 * The host expands each of these to the physical pins it occupies and
 * checks THOSE, because two claims with unrelated names can be the same
 * copper -- MDL_RES_UART(2) and MDL_RES_GPIO(2) both want PA3 on this
 * board, and no amount of comparing names would notice.
 *
 * NOTE: declaring a peripheral reserves it and is checked for conflicts,
 * but the vtable does not yet expose I2C/UART/timer operations -- an MDL
 * cannot drive them, only claim them. The arbitration mechanism landed
 * first on purpose; the drivers are the next step.
 */
#define MDL_RES_I2C(n)   { MDL_RES_KIND_I2C,   (uint8_t)(n), 0, 0 }
#define MDL_RES_UART(n)  { MDL_RES_KIND_UART,  (uint8_t)(n), 0, 0 }
#define MDL_RES_TIMER(n) { MDL_RES_KIND_TIMER, (uint8_t)(n), 0, 0 }

#define MDL_MODULE_RESOURCES(...) \
    __attribute__((used, section(".mdl_resources"))) \
    const mdl_res_t __mdl_resources[] = { __VA_ARGS__ }

/*
 * Declare the event budget this MDL needs [ABI v4]. Required if, and
 * only if, the MDL defines module_event().
 *
 *     MDL_MODULE_EVENTS(8, 200);   // 8 queue slots, expect <= 200/s
 *
 * DEPTH is checked at PACK TIME against the host's fixed per-slot event
 * budget, and an MDL asking for more than exists is refused there -- a
 * static number against a static limit, decided before anything is ever
 * pushed to a device.
 *
 * RATE cannot be checked that way and it is worth being exact about why:
 * how fast a button gets pressed is a fact about the world, not about
 * the image, so nothing in the .mdl can prove a queue will not overflow.
 * It is a CONTRACT. The host measures the arriving rate and, when it
 * exceeds this number, says 'declared 200/s, saw 3000/s' -- naming the
 * broken promise instead of reporting a mysterious loss of events. Pass
 * 0 to opt out of the check and accept silent merging.
 */
#define MDL_MODULE_EVENTS(depth, rate_hz) \
    __attribute__((used, section(".mdl_events"))) \
    const uint16_t __mdl_events[2] = { (uint16_t)(depth), (uint16_t)(rate_hz) }

/*
 * int module_event(const host_api_t *host, const mdl_event_t *evt);
 *
 * Called once per event, in the MDL's own task, UNPRIVILEGED -- the same
 * context module_init() ran in, never in interrupt context. The host's
 * ISR is small, privileged, flashed firmware; it captures the event and
 * wakes this task. That split is not a design preference: an ISR runs in
 * handler mode, which is always privileged, so calling MDL code from one
 * would execute it privileged and the sandbox would be worth nothing.
 *
 * The price is latency: an event reaches module_event() after a task
 * wake and a context switch, not at interrupt speed. Anything needing a
 * deadline tighter than that has to be host code -- no MDL can meet it,
 * and no amount of ABI design changes that.
 *
 * Return value is ignored today.
 */

typedef struct host_api {
    uint32_t abi_ver; /* Always HOST_API_ABI_VERSION for the struct
                        * layout you are compiling against. Set by the
                        * host before module_init() is called; a module
                        * should assert this matches the value it was
                        * built with (packer.py already enforces this at
                        * pack time, so a mismatch here would mean the
                        * running firmware's host_api.h and the module's
                        * are out of sync -- treat it as fatal). */

    /*
     * log(msg)
     *   Writes a diagnostic line to the host's log sink (in v1: USB CDC
     *   / semihosting, whichever the firmware build enables).
     *   - msg: NUL-terminated string. Must point inside the module's own
     *     data/heap/stack region (checked by the SVC handler in M2 --
     *     see host_api.h's validation note below); a string literal in
     *     the module's own .rodata (part of its text region) qualifies.
     *   - Length: truncated at an implementation-defined limit (v1: 127
     *     bytes) rather than rejected -- a too-long message still logs,
     *     just cut short.
     *   - Callable from module_init() and from the module's task body.
     *   - Never blocks longer than it takes to enqueue the line; does
     *     not wait for the line to actually be transmitted over USB.
     */
    void (*log)(const char *msg);

    /*
     * gpio_set(pin, level)
     *   - pin: must be one of the pins in this firmware build's
     *     compile-time GPIO whitelist (see host/host_api.c's
     *     g_gpio_whitelist). Any other value returns -1 and touches no
     *     hardware -- it does NOT fault the module, so check the return
     *     value rather than relying on a fault to catch a bad pin.
     *   - level: 0 or 1. Any other value is treated as "nonzero -> 1".
     *   - Returns 0 on success, -1 if pin is not whitelisted.
     *   - Callable from module_init() and from the module's task body.
     *   - A pin is never implicitly shared: only one loaded module may
     *     claim a given pin's whitelist slot at a time in a future
     *     multi-module build; v1 has one module slot, so this doesn't
     *     yet bite, but don't assume future versions let two modules
     *     drive the same pin.
     */
    int (*gpio_set)(int pin, int level);

    /*
     * gpio_get(pin)
     *   - pin: same whitelist rule as gpio_set.
     *   - Returns 0 or 1 (the pin's input level) on success, -1 if pin
     *     is not whitelisted. -1 is not a valid level, so it's
     *     unambiguous as an error signal here.
     *   - Callable from module_init() and from the module's task body.
     */
    int (*gpio_get)(int pin);

    /*
     * delay_ms(ms)
     *   - Blocks the calling module task for at least ms milliseconds.
     *     This is a cooperative delay (it yields the CPU to other tasks
     *     while waiting), not a busy-loop -- it costs the module
     *     nothing but wall-clock time and priority-band scheduling
     *     fairness.
     *   - ms: 0 is legal (yields once, returns as soon as rescheduled).
     *     No enforced upper bound in v1, but see the software watchdog
     *     note: a module that never returns from a host call for longer
     *     than the watchdog period gets killed exactly as if it were
     *     spinning in its own code, so passing an enormous ms is
     *     equivalent to hanging.
     *   - Callable from module_init() (from M2 on, module_init() itself
     *     runs inside the module's own FreeRTOS task -- see
     *     mdl/core/module_task.c -- so there is always a task context to
     *     yield from) and from the module's task body.
     */
    void (*delay_ms)(uint32_t ms);

    /*
     * alloc(n) / free(p)
     *   - Allocates from this module's own private pool, carved out of
     *     its heap+stack arena region -- never the host's own heap.
     *     alloc() returns NULL if the pool is exhausted or n is larger
     *     than the pool's total capacity; it does not fault, and it
     *     does not touch any other module's or the host's memory.
     *   - free(p): p must be a pointer previously returned by this same
     *     module's alloc() and not already freed. Freeing a foreign or
     *     already-freed pointer is rejected by the host's bookkeeping
     *     (parameter validation, M2) rather than corrupting the pool.
     *   - The entire pool is force-reclaimed when the module is
     *     unloaded, whether or not the module called free() on
     *     everything -- a module leaking its own allocations only wastes
     *     its own pool budget until unload, it can't leak host memory.
     *   - Callable from module_init() and from the module's task body.
     */
    void *(*alloc)(size_t n);
    void  (*free)(void *p);

    /*
     * uptime_ms()
     *   - Milliseconds since host boot. Monotonic, wraps at UINT32_MAX
     *     (~49.7 days) -- do not assume it never wraps; compute
     *     durations with subtraction (wraparound-safe for uint32_t),
     *     never with a direct greater-than comparison against a stored
     *     absolute deadline.
     *   - Callable from module_init() and from the module's task body.
     */
    uint32_t (*uptime_ms)(void);

    /*
     * atoi(s)  [ABI v2]
     *   - Decimal string to int, optional leading '-', stops at the
     *     first non-digit. No errno, no overflow report: out-of-range
     *     input saturates rather than wrapping.
     *   - s must lie in this module's own memory, same rule as log().
     *     Returns 0 for a rejected or empty string, so a module cannot
     *     tell a refused pointer from the string "0" -- callers that
     *     care should validate the text themselves.
     *   - Exists because modules build -nostdlib and every one of them
     *     that takes a console argument would otherwise hand-roll this.
     */
    int (*atoi)(const char *s);

    /*
     * watchdog_feed()  [ABI v3]
     *   - Tells the host this MDL is still making progress. The host
     *     kills an MDL that has neither fed nor been blocked in a host
     *     call for MDL_WATCHDOG_TIMEOUT_MS.
     *   - You need it only for a stretch of work longer than that
     *     timeout with no delay_ms() in it. A loop that sleeps each
     *     iteration is already covered: returning from delay_ms() counts.
     *   - Calling it does NOT make an MDL immortal -- it is a statement
     *     that you are progressing, and a module that lies about that
     *     is a module that can hang the slot until someone unloads it.
     *   - Callable from module_init(), module_cmd() and the task body.
     */
    void (*watchdog_feed)(void);

    /*
     * cycles()  [ABI v6]
     *   - Free-running CPU cycle counter (DWT CYCCNT). At 288MHz one
     *     count is ~3.5ns, and it wraps every ~15 seconds -- compute
     *     durations by subtraction, which is wraparound-safe, never by
     *     comparing two absolute values.
     *   - Reading it costs a gated call like anything else, so timing a
     *     single host call with it measures mostly itself. Time a loop of
     *     several hundred and divide.
     *   - Exists so 'almost no performance cost' can be a measurement
     *     taken from inside the sandbox rather than a claim.
     */
    uint32_t (*cycles)(void);

    /*
     * I2C [ABI v7]. bus is the instance number, so i2c_write(1, ...) is
     * I2C1 -- the same number MDL_RES_I2C(1) declares, and a bus you did
     * not declare is refused on every call, not merely at load.
     *
     * addr7 is the 7-bit address as printed in a datasheet; the shift is
     * the host's business. Buffers must lie inside your own memory, and
     * a transfer is capped at 256 bytes so that check stays meaningful.
     *
     * Returns 0, or:
     *   -1  refused -- bus not declared, bad address, buffer not yours
     *   -2  nothing answered in time (unplugged? wrong address?)
     *   -3  the transfer started and failed (NACK, arbitration, bus error)
     * -2 and -3 are kept apart because they send you to different places:
     * one is a wiring or address question, the other is not.
     *
     * These BLOCK, for up to 100ms. That is safe and deliberate: they run
     * in your own task, which is the lowest priority in the system, so
     * the supervisor and USB keep going. Returning from one counts as a
     * watchdog feed, like delay_ms().
     */
    int (*i2c_write)(int bus, int addr7, const void *data, uint32_t len);
    int (*i2c_read)(int bus, int addr7, void *data, uint32_t len);

    /* Write then read with a repeated START -- no STOP in between.
     * This is what almost every device actually needs ('write the
     * register number, then read it'), and doing it as two separate
     * calls inserts a STOP that makes many devices reset their address
     * pointer, so the two-call version silently reads the wrong
     * register. */
    int (*i2c_write_read)(int bus, int addr7, const void *tx, uint32_t txlen,
                           void *rx, uint32_t rxlen);

    /*
     * UART [ABI v8]. bus is the instance, so uart_write(2, ...) is
     * USART2 -- the number MDL_RES_UART(2) declares.
     *
     * A UART is not I2C. There is no transaction: bytes arrive whether
     * or not anyone is listening, so the host runs a receive interrupt
     * into a ring buffer and read() takes whatever has accumulated. That
     * is why read() does NOT block and returns a count -- blocking until
     * n bytes arrive would be a promise the wire cannot keep, and a
     * reader that has to guess how much is coming is the usual way
     * serial protocols deadlock.
     *
     * Declare MDL_MODULE_EVENTS() as well and MDL_EVT_UART wakes the MDL
     * when bytes land, so it need not poll. Events coalesce, which is
     * right here: what matters is that data exists, not how many times
     * that became true.
     */

    /* Baud rate, 8N1. Call before the first transfer; the host defaults
     * to 115200 if you never do. Returns 0, or -1 if the bus is not
     * yours. */
    int (*uart_config)(int bus, uint32_t baud);

    /* Blocking, bounded. Returns 0 on success, -1 refused, -2 timeout. */
    int (*uart_write)(int bus, const void *data, uint32_t len);

    /* Non-blocking. Returns the number of bytes copied (0 if none are
     * waiting), or -1 if refused. Bytes dropped because the MDL did not
     * read fast enough are counted and reported by `status`, never
     * silently discarded. */
    int (*uart_read)(int bus, void *data, uint32_t maxlen);

    /* Half-duplex self-test: TX and RX share the TX pin, so the
     * peripheral receives its own output. For proving the receive path
     * works without another device on the wire -- not a mode to talk to
     * anything real in, since every byte sent comes back. */
    int (*uart_loopback)(int bus, int enable);
} host_api_t;

/*
 * Host-side (not module-facing) support functions -- these are NOT part
 * of the module ABI surface and a module never sees their declarations
 * or calls them. main.c/loader-integration code uses these directly.
 */
struct module; /* registry.h's module_t, kept opaque here */

/* The one host_api_t instance handed to every loaded module in v1
 * (single module slot -> one vtable instance is enough). Populated once
 * at boot by host_api_init(). */
extern const host_api_t g_host_api;

void host_api_init(void);

/* Resets this module's private alloc() pool to empty, carved out of
 * m's own heap+stack arena region (see host_api.c's HEAP_FRACTION
 * comment for the heap/stack split). Call once, after a successful
 * mdl_load() and before mdl_run() -- loader.c itself doesn't call this,
 * since core/ doesn't know host_api.c exists; the platform's main.c
 * sequences load -> host_api_pool_reset -> run explicitly. */
void host_api_pool_reset(struct module *m);

/*
 * Host-side GPIO access to the same whitelist the module vtable exposes,
 * for host code that is already privileged (the debug console in
 * mdl/transport/console.c).
 *
 * These are NOT the vtable entries with the SVC gate removed as an
 * optimisation -- the difference that matters is that they do NOT feed
 * the software watchdog. Routing a console "led 0 1" through
 * g_host_api.gpio_set() would bump g_mdl_slot.last_active_tick and so
 * convince mdl_supervisor_run() that a module stuck in while(1){} was
 * still alive, purely because a human typed at the console. Same pin
 * indices and same -1-on-not-whitelisted return as gpio_set()/gpio_get().
 */
/* Which host subsystem owns a whitelist pin, or NULL if a module may
 * claim it -- what the console's `pins` command prints in its owner
 * column, and the same table mdl_res_owner() answers load-time claims
 * from. */
/* Return every pin in `claimed` (a gpio_claimed bitmap) to its default
 * state. The supervisor calls this on unload, before clearing the
 * bitmap -- see host_gpio_release_claims()'s own comment for why the
 * full configuration is re-applied and not merely the output level. */
uint32_t host_gpio_release_claims(uint32_t claimed); /* -> pins actually restored */

/* Which EXINT line/vector a whitelist pin is on; false if it cannot
 * raise an interrupt. host_events.c uses this to arm a declared claim. */
bool host_gpio_exti_info(int pin, uint8_t *port_source, uint8_t *pin_source,
                          uint32_t *line, int *irqn);

/* The physical pin (MDL_PIN space) behind a whitelist index. */
/* Ungated cycle read, for host code (the event ISR stamps events with
 * it). The gated host_cycles() is the module-facing one. */
uint32_t host_cycles_now(void);

/* Is this buffer entirely inside the loaded MDL's own memory? Exported
 * so the peripheral drivers validate with the same code the vtable does
 * -- two implementations of this boundary would eventually disagree. */
bool host_ptr_owned_by_module(const void *p, size_t len);

/* Bring up / tear down an I2C bus an MDL has claimed. Called by the
 * supervisor around load and unload, alongside the GPIO equivalents. */
bool host_i2c_claim(int instance);
void host_i2c_release(int instance);
bool host_uart_claim(int instance);
void host_uart_release(int instance);
/* Bytes lost because the MDL did not read fast enough, for `status`. */
uint32_t host_uart_dropped(int instance);

bool host_gpio_pin_id(int pin, uint8_t *out);

const char *host_gpio_host_owner(int pin);

/* True while a module has declared a pin the host would otherwise be
 * using itself, so the host's own indicator task can stand down rather
 * than fight it. A collision becomes a handover precisely because the
 * module said so up front. */
bool host_gpio_yielded_to_module(int pin);

int host_gpio_direct_set(int pin, int level);
int host_gpio_direct_get(int pin);

/* Human-readable name for a whitelist pin index ("LEDB (PD10)"), or NULL
 * if the index isn't whitelisted -- so the console can list what a module
 * is actually allowed to touch without duplicating the board pin table. */
const char *host_gpio_name(int pin);

#endif /* MDL_HOST_API_H */
